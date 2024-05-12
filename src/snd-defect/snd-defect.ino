#include <SD.h>
#include "driver/i2s.h"
#include <Preferences.h>
#include <vector>
#include <strings.h>
#include "esp_task_wdt.h"


	char enter1[512] = "#rand10,99 #pause500 #TONE250,99 #PAUSE500 #TONE250,8 #PAUSE500 #TONE250,6 #PAUSE500 #TONE250,4 #PAUSE500 #TONE250,2 #PAUSE500 #TONE250,0 #PAUSE500 point 5";
	char enter2[512] = "#rand40,10";
	char enter3[512] = "4 4 4 point 4";
	char enter4[512] = "7 7 7 point 7";


// 3 sec WDT
#define WDT_TIMEOUT 3

#include "sound.h"
#include "vocab/vocab.h"

// Samples for each buffer
#define AUDIO_BUFFER_SIZE 512
#define AUDIO_BUFFER_NUM	4

i2s_port_t i2s_num = I2S_NUM_0;

// Bytes
#define FILE_BUFFER_SIZE 2048

// Pins
#define EN1       9
#define EN2       10
#define EN3       3
#define EN4       21
#define LEDA      11
#define LEDB      12
#define VOLDN     13
#define VOLUP     14
#define AUX1      15
#define AUX2      16
#define AUX3      17
#define AUX4      18
#define AUX5      8
#define I2S_SD    4
#define I2S_DATA  5
#define I2S_BCLK  6
#define I2S_LRCLK 7
#define SDCLK     36
#define SDMOSI    35
#define SDMISO    37
#define SDCS      34
#define SDDET     33

// Bit positions for inputs
#define VOL_UP_BUTTON 0x01
#define VOL_DN_BUTTON 0x02
#define EN1_INPUT     0x10
#define EN2_INPUT     0x20
#define EN3_INPUT     0x40
#define EN4_INPUT     0x80

// Debounce in 10ms increments
#define RISE_DEBOUNCE 50
#define FALL_DEBOUNCE 300

volatile uint8_t buttonsPressed = 0;
volatile bool riseEn1 = false;
volatile bool riseEn2 = false;
volatile bool riseEn3 = false;
volatile bool riseEn4 = false;
volatile bool fallEn1 = false;
volatile bool fallEn2 = false;
volatile bool fallEn3 = false;
volatile bool fallEn4 = false;

// Volume
#define VOL_STEP_MAX	30
#define VOL_STEP_NOM	20

uint8_t volumeStep = 0;
volatile uint16_t volume = 0;
uint8_t volumeUpCoef = 10;
uint8_t volumeDownCoef = 8;

uint16_t volumeLevels[] = {
	0,			// 0
	100,
	200,
	300,
	400,
	500,
	600,
	700,
	800,
	900,
	1000,	 // 10
	1900,
	2800,
	3700,
	4600,
	5500,
	6400,
	7300,
	8200,
	9100,
	10000,	// 20
	11000,
	12000,
	13000,
	14000,
	15000,
	16000,
	17000,
	18000,
	19000,
	20000,	// 30
};

bool restart = false;

uint8_t enableAudio = 0;							// 0 or 1

uint8_t noiseLevel = 0;
uint8_t lowPassCoef = 10;
uint8_t highPassCoef = 10;

Preferences preferences;

uint8_t debounce(uint8_t debouncedState, uint8_t newInputs)
{
	static uint8_t clock_A = 0, clock_B = 0;
	uint8_t delta = newInputs ^ debouncedState;	 //Find all of the changes
	uint8_t changes;

	clock_A ^= clock_B;										 //Increment the counters
	clock_B	= ~clock_B;

	clock_A &= delta;											 //Reset the counters if no changes
	clock_B &= delta;											 //were detected.

	changes = ~((~delta) | clock_A | clock_B);
	debouncedState ^= changes;
	return(debouncedState);
}

char* rtrim(char* in)
{
	char* endPtr = in + strlen(in) - 1;
	while (endPtr >= in && isspace(*endPtr))
		*endPtr-- = 0;

	return in;
}

char* ltrim(char* in)
{
	char* startPtr = in;
	uint32_t bytesToMove = strlen(in);
	while(isspace(*startPtr))
		startPtr++;
	bytesToMove -= (startPtr - in);
	memmove(in, startPtr, bytesToMove);
	in[bytesToMove] = 0;
	return in;
}

bool configKeyValueSplit(char* key, uint32_t keySz, char* value, uint32_t valueSz, const char* configLine)
{
	char lineBuffer[256];
	char* separatorPtr = NULL;
	char* lineBufferPtr = NULL;
	uint32_t bytesToCopy;

	separatorPtr = strchr(configLine, '=');
	if (NULL == separatorPtr)
		return false;

	memset(key, 0, keySz);
	memset(value, 0, valueSz);

	// Copy the part that's eligible to be a key into the line buffer
	bytesToCopy = separatorPtr - configLine;
	if (bytesToCopy > sizeof(lineBuffer)-1)
		bytesToCopy = sizeof(lineBuffer);
	memset(lineBuffer, 0, sizeof(lineBuffer));
	strncpy(lineBuffer, configLine, bytesToCopy);

	lineBufferPtr = ltrim(rtrim(lineBuffer));
	if (0 == strlen(lineBufferPtr) || '#' == lineBufferPtr[0])
		return false;

	strncpy(key, lineBufferPtr, keySz);

//	bytesToCopy = strlen(separatorPtr+1);
//	if (bytesToCopy > sizeof(lineBuffer)-1)
//		bytesToCopy = sizeof(lineBuffer);
	memset(lineBuffer, 0, sizeof(lineBuffer));
	// Changed to sizeof(lineBuffer)-1 below instead of bytesToCopy due to -Werror=stringop-overflow and -Werror=stringop-truncation
	strncpy(lineBuffer, separatorPtr+1, sizeof(lineBuffer)-1);
	lineBufferPtr = ltrim(rtrim(lineBuffer));
	if (0 == strlen(lineBufferPtr))
	{
		memset(key, 0, keySz);
		return false;
	}
	strncpy(value, lineBufferPtr, valueSz);
	return true;
}

hw_timer_t * timer = NULL;

void IRAM_ATTR processVolume(void)
{
	static uint8_t oldButtonsPressed = 0;
	static uint8_t buttonsDebounced = 0;
	static uint32_t debounceCount1 = 0;
	static unsigned long pressTime = 0;
	uint8_t inputStatus = 0;

	// Turn off LED
	uint16_t ledHoldTime = (VOL_STEP_NOM == volumeStep) ? 1000 : 100;
	if((millis() - pressTime) > ledHoldTime)
	{
		digitalWrite(LEDB, 0);
	}

	// Read inputs
	if(digitalRead(VOLUP))
		inputStatus &= ~VOL_UP_BUTTON;
	else
		inputStatus |= VOL_UP_BUTTON;

	if(digitalRead(VOLDN))
		inputStatus &= ~VOL_DN_BUTTON;
	else
		inputStatus |= VOL_DN_BUTTON;

	if(digitalRead(EN1))
		inputStatus &= ~EN1_INPUT;
	else
		inputStatus |= EN1_INPUT;

	if(digitalRead(EN2))
		inputStatus &= ~EN2_INPUT;
	else
		inputStatus |= EN2_INPUT;

	if(digitalRead(EN3))
		inputStatus &= ~EN3_INPUT;
	else
		inputStatus |= EN3_INPUT;

	if(digitalRead(EN4))
		inputStatus &= ~EN4_INPUT;
	else
		inputStatus |= EN4_INPUT;

	// Debounce
	buttonsPressed = debounce(buttonsPressed, inputStatus);

	// Find rising edge of volume up button
	if((buttonsPressed ^ oldButtonsPressed) & (buttonsPressed & VOL_UP_BUTTON))
	{
		pressTime = millis();
		if(volumeStep < VOL_STEP_MAX)
		{
			volumeStep++;
			preferences.putUChar("volume", volumeStep);
		}
		Serial.print("Vol Up: ");
		Serial.println(volumeStep);
		digitalWrite(LEDB, 1);
	}

	// Find rising edge of volume down button
	if((buttonsPressed ^ oldButtonsPressed) & (buttonsPressed & VOL_DN_BUTTON))
	{
		pressTime = millis();
		if(volumeStep > 0)
		{
			volumeStep--;
			preferences.putUChar("volume", volumeStep);
		}
		Serial.print("Vol Dn: ");
		Serial.println(volumeStep);
		digitalWrite(LEDB, 1);
	}

	// Check for serial input
	if(Serial.available() > 0)
	{
		uint8_t serialChar = Serial.read();
		switch(serialChar)
		{
			case 'a':
				if(noiseLevel < 255)
				{
					noiseLevel++;
					preferences.putUChar("noiseLevel", noiseLevel);
				}
				Serial.print("Noise Level: ");
				Serial.print(noiseLevel * 100);
				Serial.println("");
				break;
			case 'z':
				if(noiseLevel > 0)
				{
					noiseLevel--;
					preferences.putUChar("noiseLevel", noiseLevel);
				}
				Serial.print("Noise Level: ");
				Serial.print(noiseLevel * 100);
				Serial.println("");
				break;

			case 's':
				if(lowPassCoef < 10)
				{
					lowPassCoef++;
					preferences.putUChar("lowPassCoef", lowPassCoef);
				}
				Serial.print("Low Pass Coef: ");
				Serial.print(lowPassCoef);
				Serial.println("");
				break;
			case 'x':
				if(lowPassCoef > 0)
				{
					lowPassCoef--;
					preferences.putUChar("lowPassCoef", lowPassCoef);
				}
				Serial.print("Low Pass Coef: ");
				Serial.print(lowPassCoef);
				Serial.println("");
				break;

			case 'd':
				if(highPassCoef < 10)
				{
					highPassCoef++;
					preferences.putUChar("highPassCoef", highPassCoef);
				}
				Serial.print("High Pass Coef: ");
				Serial.print(highPassCoef);
				Serial.println("");
				break;
			case 'c':
				if(highPassCoef > 0)
				{
					highPassCoef--;
					preferences.putUChar("highPassCoef", highPassCoef);
				}
				Serial.print("High Pass Coef: ");
				Serial.print(highPassCoef);
				Serial.println("");
				break;

			case 'q':
				restart = true;
				break;
		}
	}

	if(buttonsPressed & (EN1_INPUT | EN2_INPUT | EN3_INPUT | EN4_INPUT))
	{
		digitalWrite(LEDA, 1);
	}
	else
	{
		digitalWrite(LEDA, 0);
	}


	if(buttonsDebounced & EN1_INPUT)
	{
		// EN1 state high, looking for low
		if(!(buttonsPressed & EN1_INPUT))
		{
			// EN1 input low
			if(debounceCount1)
				debounceCount1--;
			else
			{
				// Debounce expired, switch state
				buttonsDebounced &= ~EN1_INPUT;
				fallEn1 = true;
				debounceCount1 = RISE_DEBOUNCE;
			}
		}
		else
		{
			// EN1 input high, reset
			debounceCount1 = FALL_DEBOUNCE;
		}
	}
	else
	{
		// EN1 state low, looking for high
		if(buttonsPressed & EN1_INPUT)
		{
			// EN1 input high
			if(debounceCount1)
				debounceCount1--;
			else
			{
				// Debounce expired, switch state
				buttonsDebounced |= EN1_INPUT;
				riseEn1 = true;
				debounceCount1 = FALL_DEBOUNCE;
			}
		}
		else
		{
			// EN1 input low, reset
			debounceCount1 = RISE_DEBOUNCE;
		}
	}


	// Find rising edge of EN2
	if((buttonsPressed ^ oldButtonsPressed) & (buttonsPressed & EN2_INPUT))
	{
		riseEn2 = true;
	}

	// Find falling edge of EN2
	if((buttonsPressed ^ oldButtonsPressed) & ~(buttonsPressed | ~EN2_INPUT))
	{
		fallEn2 = true;
	}

	// Find rising edge of EN3
	if((buttonsPressed ^ oldButtonsPressed) & (buttonsPressed & EN3_INPUT))
	{
		riseEn3 = true;
	}

	// Find falling edge of EN3
	if((buttonsPressed ^ oldButtonsPressed) & ~(buttonsPressed | ~EN3_INPUT))
	{
		fallEn3 = true;
	}

	// Find rising edge of EN4
	if((buttonsPressed ^ oldButtonsPressed) & (buttonsPressed & EN4_INPUT))
	{
		riseEn4 = true;
	}

	// Find falling edge of EN4
	if((buttonsPressed ^ oldButtonsPressed) & ~(buttonsPressed | ~EN4_INPUT))
	{
		fallEn4 = true;
	}

	// Process volume
	uint16_t deltaVolume;
	uint16_t volumeTarget;
	volumeTarget = volumeLevels[volumeStep] * enableAudio;

	if(volume < volumeTarget)
	{
		deltaVolume = (volumeTarget - volume);
		if((deltaVolume > 0) && (deltaVolume < volumeUpCoef))
			deltaVolume = volumeUpCoef;	// Make sure it goes all the way to min or max
		volume += deltaVolume	/ volumeUpCoef;
//		Serial.println(volume);
	}
	else if(volume > volumeTarget)
	{
		deltaVolume = (volume - volumeTarget);
		if((deltaVolume > 0) && (deltaVolume < volumeDownCoef))
			deltaVolume = volumeDownCoef;	// Make sure it goes all the way to min or max
		volume -= deltaVolume / volumeDownCoef;
//		Serial.println(volume);
	}

	oldButtonsPressed = buttonsPressed;
}

void setup()
{
	// Open serial communications and wait for port to open:
	Serial.begin(115200);

	pinMode(VOLDN, INPUT_PULLUP);
	pinMode(VOLUP, INPUT_PULLUP);
	pinMode(I2S_SD, OUTPUT);
	digitalWrite(I2S_SD, 0);	// Disable amplifier

	pinMode(LEDA, OUTPUT);
	pinMode(LEDB, OUTPUT);
	digitalWrite(LEDA, 0);
	digitalWrite(LEDB, 0);

	pinMode(EN1, INPUT_PULLUP);
	pinMode(EN2, INPUT_PULLUP);
	pinMode(EN3, INPUT_PULLUP);
	pinMode(EN4, INPUT_PULLUP);

	pinMode(AUX1, OUTPUT);
	pinMode(AUX2, OUTPUT);
	pinMode(AUX3, OUTPUT);
	pinMode(AUX4, OUTPUT);
	pinMode(AUX5, OUTPUT);

	esp_task_wdt_init(WDT_TIMEOUT, true);
	esp_task_wdt_add(NULL); //add current thread to WDT watch
	esp_task_wdt_reset();

	timer = timerBegin(0, 80, true);	// Timer 0, 80x prescaler = 1us
	timerAttachInterrupt(timer, &processVolume, false);	// level triggered
	timerAlarmWrite(timer, 10000, true);	// 80MHz / 80 / 10000 = 10ms, autoreload

	i2s_config_t i2s_config = {
			.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
			.sample_rate = 16000,
			.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
			.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
			.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
			.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
			.dma_buf_count = AUDIO_BUFFER_NUM,
			.dma_buf_len = AUDIO_BUFFER_SIZE,
			.use_apll = 0,
			.tx_desc_auto_clear = true,
			.fixed_mclk = -1,
			.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
			.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
	};

	i2s_pin_config_t pin_config = {
			.mck_io_num = I2S_PIN_NO_CHANGE,
			.bck_io_num = I2S_BCLK,
			.ws_io_num = I2S_LRCLK,
			.data_out_num = I2S_DATA,
			.data_in_num = I2S_PIN_NO_CHANGE,
	};

	i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
	i2s_set_pin(i2s_num, &pin_config);
}

void enableAmplifier(void)
{
	digitalWrite(I2S_SD, 1);	// Enable amplifier
	esp_task_wdt_reset();
}

void disableAmplifier(void)
{
	size_t i;
	uint32_t outputValue;
	size_t bytesWritten;

	for(i=0; i<(AUDIO_BUFFER_NUM*AUDIO_BUFFER_SIZE); i++)
	{
		// Fill all buffers with zeros
		esp_task_wdt_reset();
		outputValue = 0;
		i2s_write(i2s_num, &outputValue, 4, &bytesWritten, portMAX_DELAY);
	}
	digitalWrite(I2S_SD, 0);	// Disable amplifier
}

int16_t generateNoise(void)
{
	static int16_t noiseValue = 0;  // static so the value and filter are consistent over time
	static int16_t noiseValue1 = 0;
	static int16_t noisePrevious = 0;

	int16_t noise = random(0, noiseLevel * 100) - (noiseLevel * 50);
	noiseValue1 = (noiseValue + (noise - noisePrevious)) * highPassCoef / 10;   // high pass: alpha = fs / (2*pi*fc + fs)
	noiseValue = noiseValue + (noiseValue1 - noiseValue) * lowPassCoef / 10;    // low pass:  alpha = 2*pi*fc / (2*pi*fc + fs))
	noisePrevious = noise;
	
	return noiseValue;
}

void sendSampleToI2S(int32_t sampleValue)
{
	uint32_t outputValue;
	size_t bytesWritten;

	sampleValue += generateNoise();
	int32_t adjustedValue = sampleValue * volume / volumeLevels[VOL_STEP_NOM];
	if(adjustedValue > 32767)
		sampleValue = 32767;
	else if(adjustedValue < -32768)
		sampleValue = -32768;
	else
		sampleValue = adjustedValue;
	// Combine into 32 bit word (left & right)
	outputValue = (sampleValue<<16) | (sampleValue & 0xffff);
	i2s_write(i2s_num, &outputValue, 4, &bytesWritten, portMAX_DELAY);
}

void play(Sound *wavSound)
{
	size_t i;
	size_t bytesRead;
	uint8_t fileBuffer[FILE_BUFFER_SIZE];

	esp_task_wdt_reset();

	wavSound->open();
	
	while(wavSound->available())
	{
		esp_task_wdt_reset();
		bytesRead = wavSound->read(fileBuffer, (size_t)FILE_BUFFER_SIZE);
		// Audio buffer samples are in 16-bit chunks, so step by two
		for(i=0; i<bytesRead; i+=2)
		{
			esp_task_wdt_reset();
			// File is read on a byte basis, so convert into int16 samples, and step every 2 bytes
			int32_t sampleValue = *((int16_t *)(fileBuffer+i));
			sendSampleToI2S(sampleValue);
		}
		if(restart)
			break;	// Stop playing and return to the main loop
	}

	wavSound->close();
}


int16_t sineWave[8] = {0, 12539, 23170, 30273, 32767, 30273, 23170, 12539};

void playTone(uint8_t decisecs, uint8_t amplitude)
{
	uint32_t i, j;
	bool invert = false;
	int32_t sampleValue;
	
	esp_task_wdt_reset();

	for(i=0; i<(2*100); i++)  // Each cycle is 1ms, so play 200 half cycles
	{
		esp_task_wdt_reset();
		for(j=0; j<8; j++)
		{
			if(invert)
				sampleValue = -sineWave[j];
			else
				sampleValue = sineWave[j];
			sampleValue = sampleValue * amplitude / 8;
			sendSampleToI2S(sampleValue);
		}
		invert = !invert;
		if(restart)
			break;	// Stop playing and return to the main loop
	}
}


void playSilence(uint8_t decisecs)
{
	uint32_t i;
	uint32_t totalSamples;
	
	esp_task_wdt_reset();

	totalSamples = 1600 * decisecs;

	for(i=0; i<totalSamples; i++)
	{
		esp_task_wdt_reset();
		sendSampleToI2S(0);
		if(restart)
			break;	// Stop playing and return to the main loop
	}
}


void loop()
{
	bool usingSdSounds = false;
	size_t fileNameLength;
	File rootDir;
	File wavFile;
	const char *fileName;
	uint16_t channels = 0;
	uint32_t sampleRate = 0;
	uint16_t bitsPerSample = 0;
	uint32_t wavDataSize = 0;
	size_t i;

	std::vector<Sound *> vocab;

	esp_task_wdt_reset();
	timerAlarmDisable(timer);

	Serial.println("\nD E F E C T   D E T E C T O R\n");

	Serial.print("Version: ");
	Serial.println(VERSION_STRING);

	Serial.print("Git Rev: ");
	Serial.println(GIT_REV, HEX);

	// Read NVM configuration
	preferences.begin("squeal", false);
	volumeStep = preferences.getUChar("volume", VOL_STEP_NOM);
	noiseLevel = preferences.getUChar("noiseLevel", 50);
	lowPassCoef = preferences.getUChar("lowPassCoef", 10);
	highPassCoef = preferences.getUChar("highPassCoef", 10);

	esp_task_wdt_reset();

	// Check SD card
	if(SD.begin())
	{
		// Check for and read config file
		File f = SD.open("/config.txt");
		if (f)
		{
			while(f.available())
			{
				char keyStr[128];
				char valueStr[128];
				bool kvFound = configKeyValueSplit(keyStr, sizeof(keyStr), valueStr, sizeof(valueStr), f.readStringUntil('\n').c_str());
				if (!kvFound)
					continue;

				// Okay, looks like we have a valid key/value pair, see if it's something we care about
				if (0 == strcmp(keyStr, "noiseLevel"))
				{
					noiseLevel = atoi(valueStr) / 100;
				}
			}
		}
		f.close();

		// Find WAV files
		rootDir = SD.open("/");
		while(true)
		{
			esp_task_wdt_reset();
			wavFile = rootDir.openNextFile();

			if (!wavFile)
			{
				break;	// No more files
			}
			if(wavFile.isDirectory())
			{
				Serial.print("	Skipping directory: ");
				Serial.println(wavFile.name());
			}
			else
			{
				fileName = wavFile.name();
				fileNameLength = strlen(fileName);
				if(fileNameLength < 5)
					continue;	// Filename too short (x.wav = min 5 chars)
				const char *extension = &fileName[strlen(fileName)-4];
				if(strcasecmp(extension, ".wav"))
				{
					Serial.print("	Ignoring: ");
					Serial.println(fileName);
					continue;	// Not a wav file (by extension anyway)
				}
				
				if(!wavFile.find("fmt "))	// Includes trailing space
				{
					Serial.print("! No fmt section: ");
					Serial.println(fileName);
					continue;
				}

				wavFile.seek(wavFile.position() + 6);	// Seek to number of channels
				wavFile.read((uint8_t*)&channels, 2);	// Read channels - WAV is little endian, only works if uC is also little endian

				if(channels > 1)
				{
					Serial.print("! Not mono: ");
					Serial.println(fileName);
					continue;
				}

				wavFile.read((uint8_t*)&sampleRate, 4);	// Read sample rate - WAV is little endian, only works if uC is also little endian

				if((8000 != sampleRate) && (16000 != sampleRate) && (32000 != sampleRate) && (44100 != sampleRate))
				{
					Serial.print("! Incorrect sample rate: ");
					Serial.println(fileName);
					continue;
				}

				wavFile.seek(wavFile.position() + 6);	// Seek to bits per sample
				wavFile.read((uint8_t*)&bitsPerSample, 2);	// Read bits per sample - WAV is little endian, only works if uC is also little endian

				if(16 != bitsPerSample)
				{
					Serial.print("! Not 16-bit: ");
					Serial.println(fileName);
					continue;
				}

				if(!wavFile.find("data"))
				{
					Serial.print("! No data section: ");
					Serial.println(fileName);
					continue;
				}

				// If we got here, then it looks like a valid wav file
				// Get data length and offset

				wavFile.read((uint8_t*)&wavDataSize, 4);	// Read data size - WAV is little endian, only works if uC is also little endian
				// Offset is now the current position

				Serial.print("+ Adding ");
				Serial.print(fileName);
				Serial.print(" (");
				Serial.print(sampleRate);
				Serial.print(",");
				Serial.print(wavDataSize);
				Serial.print(",");
				Serial.print(wavFile.position());
				Serial.println(")");

				vocab.push_back(new SdSound(fileName, wavDataSize, wavFile.position(), sampleRate));
				usingSdSounds = true;
			}
			wavFile.close();
		}
		rootDir.close();
	}

	// Print configuration values
	Serial.print("Volume: ");
	Serial.println(volumeStep);
	Serial.print("Noise Level: ");
	Serial.println(noiseLevel * 100);
	Serial.print("Low Pass Coef: ");
	Serial.println(lowPassCoef);
	Serial.print("High Pass Coef: ");
	Serial.println(highPassCoef);

	Serial.println("");

	esp_task_wdt_reset();

	if(usingSdSounds)
	{
		Serial.print("Using SD card sounds (");
		Serial.print(vocab.size());
		Serial.println(")");
		// Quadruple blink blue
		digitalWrite(LEDA, 1); delay(250); digitalWrite(LEDA, 0); delay(250);
		esp_task_wdt_reset();
		digitalWrite(LEDA, 1); delay(250); digitalWrite(LEDA, 0); delay(250);
		esp_task_wdt_reset();
		digitalWrite(LEDA, 1); delay(250); digitalWrite(LEDA, 0); delay(250);
		esp_task_wdt_reset();
		digitalWrite(LEDA, 1); delay(250); digitalWrite(LEDA, 0); delay(250);
		esp_task_wdt_reset();
	}
	else
	{
		vocab.push_back(new MemSound("0", __0_wav, __0_wav_len, 16000));
		vocab.push_back(new MemSound("1", __1_wav, __1_wav_len, 16000));
		vocab.push_back(new MemSound("2", __2_wav, __2_wav_len, 16000));
		vocab.push_back(new MemSound("3", __3_wav, __3_wav_len, 16000));
		vocab.push_back(new MemSound("4", __4_wav, __4_wav_len, 16000));
		vocab.push_back(new MemSound("5", __5_wav, __5_wav_len, 16000));
		vocab.push_back(new MemSound("6", __6_wav, __6_wav_len, 16000));
		vocab.push_back(new MemSound("7", __7_wav, __7_wav_len, 16000));
		vocab.push_back(new MemSound("8", __8_wav, __8_wav_len, 16000));
		vocab.push_back(new MemSound("9", __9_wav, __9_wav_len, 16000));
		vocab.push_back(new MemSound("milepost", milepost_wav, milepost_wav_len, 16000));
		vocab.push_back(new MemSound("point", point_wav, point_wav_len, 16000));

		Serial.print("Using built-in sounds (");
		Serial.print(vocab.size());
		Serial.println(")");
		// Double blink blue
		digitalWrite(LEDA, 1); delay(250); digitalWrite(LEDA, 0); delay(250);
		esp_task_wdt_reset();
		digitalWrite(LEDA, 1); delay(250); digitalWrite(LEDA, 0); delay(250);
		esp_task_wdt_reset();
	}

	timerAlarmEnable(timer);

	char *str = NULL;

	while(1)
	{
		esp_task_wdt_reset();

		// FIXME: turn into a queue of events?  That way they will process in the same order as they happen.  But risk of overflowing queue.
		if(riseEn1)
		{
			Serial.println("Input 1 Enter");
			str = strdup(enter1);
			riseEn1 = false;
		}
		else if(riseEn2)
		{
			Serial.println("Input 2 Enter");
			str = strdup(enter2);
			riseEn2 = false;
		}
		else if(riseEn3)
		{
			Serial.println("Input 3 Enter");
			str = strdup(enter3);
			riseEn3 = false;
		}
		else if(riseEn4)
		{
			Serial.println("Input 4 Enter");
			str = strdup(enter4);
			riseEn4 = false;
		}

		if(fallEn1)
		{
			Serial.println("Input 1 Exit");
			fallEn1 = false;
		}
		else if(riseEn2)
		{
			Serial.println("Input 2 Exit");
			fallEn2 = false;
		}
		else if(riseEn3)
		{
			Serial.println("Input 3 Exit");
			fallEn3 = false;
		}
		else if(riseEn4)
		{
			Serial.println("Input 4 Exit");
			fallEn4 = false;
		}

		if(NULL != str)
		{
			Serial.print("Heap free: ");
			Serial.println(esp_get_free_heap_size());

			// Clean up string: lowercase
			for(int i = 0; str[i]; i++)
			{
				str[i] = tolower(str[i]);
			}


			enableAmplifier();
			enableAudio = 1;  // Enable to ramp up volume
			playSilence(5);  // Preload 500ms silence

			Serial.print("Playing... ");

			// Get first token
			char *token = strtok(str, " ");

			while( token != NULL )
			{
				bool valid = false;
				
				if('#' == token[0])
				{
					// Command
					if(!strncmp("rand", &token[1], 4))
					{
						// Random number
						char *charPtr = strchr(token, ',');
						if(NULL != charPtr)
						{
							valid = true;
							*charPtr = 0;  // Null terminate at comma
							uint32_t numA = atoi(&token[5]);
							uint32_t numB = atoi(charPtr + 1);
							char randomNumber[8];
							snprintf(randomNumber, 8, "%ld", random(numA, numB+1));
							Serial.print(" RAND(");
							Serial.print(randomNumber);
							Serial.print(")");
							
							charPtr = randomNumber;
							while(0 != *charPtr)
							{
								char testChar[2];
								testChar[0] = *charPtr;
								testChar[1] = 0;
								for(i=0; i<vocab.size(); i++)
								{
									if(vocab[i]->matchName(testChar))
									{
										play(vocab[i]);
									}
								}
								charPtr++;
							}
						}
					}
					else if(!strncmp("pause", &token[1], 5))
					{
						// Pause
						valid = true;
						uint32_t numA = atoi(&token[6]);
						numA = numA / 100;  // Convert to decisecs
						Serial.print(" PAUSE(");
						Serial.print(numA * 100);
						Serial.print(")");
						while(numA--)
						{
							playSilence(1);	
						}
					}
					else if(!strncmp("tone", &token[1], 4))
					{
						// Tone
						char *charPtr = strchr(token, ',');
						if(NULL != charPtr)
						{
							valid = true;
							*charPtr = 0;  // Null terminate at comma
							uint32_t numA = atoi(&token[5]);  // duration
							uint32_t numB = atoi(charPtr + 1);   // amplitude
							numA = numA / 100;  // Convert to decisecs
							if(numB < 1)
								numB = 1;
							else if(numB > 8)
								numB = 8;
							char randomNumber[8];
							snprintf(randomNumber, 8, "%ld", random(numA, numB+1));
							Serial.print(" TONE(");
							Serial.print(numA * 100);
							Serial.print(",");
							Serial.print(numB);
							Serial.print(")");
							while(numA--)
							{
								playTone(1, numB);	
							}
						}
					}
				}

				for(i=0; i<vocab.size(); i++)
				{
					if(vocab[i]->matchName(token))
					{
						valid = true;
						Serial.print(' ');
						Serial.print(token);
						play(vocab[i]);
					}
				}

				if(!valid)
				{
					Serial.print(" ***");
				}

				// Get next token
				token = strtok(NULL, " ");
			}

			Serial.println("");

			playSilence(5);
			enableAudio = 0;  // Disable to ramp down volume
			while(volume != 0)  // Wait for volume to ramp down
				esp_task_wdt_reset();
			disableAmplifier();
			free(str);
			str = NULL;
		}

		if(restart)
		{
			restart = false;
			Serial.print("\n*** Restarting ***\n\n");
			vocab.clear();
			break;	// Restart the loop() function
		}

	}
}

