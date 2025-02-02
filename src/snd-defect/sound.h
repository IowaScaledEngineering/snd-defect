#pragma once

class Sound
{
	protected:
		size_t dataSize;
		size_t byteCount;
		uint16_t sampleRate;
		char *namePtr;

	public:
		virtual void open(void);
		virtual size_t available(void)
		{
			if(byteCount > dataSize)
				return 0;
			else
				return(dataSize - byteCount);
		}
		virtual size_t read(uint8_t *buffer, size_t numBytes);
		virtual void close(void);
		uint16_t getSampleRate(void)
		{
			return sampleRate;
		}
		bool matchName(char *name)
		{
			return !strcmp(namePtr, name);
		}
};

class SdSound : public Sound
{
	char *fileName;
	size_t dataOffset;
	File wavFile;

	public:
		SdSound(const char *fname, size_t numBytes, size_t offset, uint16_t sr)
		{
			fileName = strdup(fname);
			// FIXME: Need to assign namePtr to parsed base filename
			dataOffset = offset;
			dataSize = numBytes;
			sampleRate = sr;
		}
		~SdSound()
		{
			free(fileName);
			free(namePtr);
		}
		void open(void)
		{
			wavFile = SD.open(String("/") + fileName);
			wavFile.seek(dataOffset);
//			Serial.print("	");
//			Serial.println(fileName);
			byteCount = 0;
		}
		size_t read(uint8_t *buffer, size_t numBytes)
		{
			size_t bytesToRead, bytesRead;
			if(available() < numBytes)
				bytesToRead = available();
			else
				bytesToRead = numBytes;

			bytesRead = wavFile.read(buffer, bytesToRead);
			byteCount += bytesRead;
			return bytesRead;
		}
		void close(void)
		{
			wavFile.close();
		}
};

class MemSound : public Sound
{
	const uint8_t *dataPtr;

	public:
		MemSound(const char *wordName, const uint8_t *sound, size_t numBytes, uint16_t sr)
		{
			namePtr = strdup(wordName);
			dataPtr = sound;
			dataSize = numBytes;
			sampleRate = sr;
		}
		~MemSound()
		{
			// No need to free dataPtr since it is const
			free(namePtr);
		}
		void open(void)
		{
			byteCount = 0;
		}
		size_t read(uint8_t *buffer, size_t numBytes)
		{
			size_t bytesToRead;
			if(available() < numBytes)
				bytesToRead = available();
			else
				bytesToRead = numBytes;
			memcpy(buffer, dataPtr+byteCount, bytesToRead);
			byteCount += bytesToRead;
			return bytesToRead;
		}
		void close(void)
		{
			return;
		}
};

class MemSoundInterp : public Sound
{
	const uint8_t *dataPtr;
	bool interpolate;

	public:
		MemSoundInterp(const char *wordName, const uint8_t *sound, size_t numBytes, uint16_t sr)
		{
			namePtr = strdup(wordName);
			dataPtr = sound;
			dataSize = numBytes;
			sampleRate = sr;
		}
		~MemSoundInterp()
		{
			// No need to free dataPtr since it is const
			free(namePtr);
		}
		void open(void)
		{
			byteCount = 0;
			interpolate = false;
		}
		size_t read(uint8_t *buffer, size_t numBytes)
		{
			// numBytes must be even; no memory protection if given an odd number (can write beyond buffer)

			// dataSize = 8
			//               S    i    S    i    S    i    S
			// SrcBytes:    0 1       2 3       4 5       6 7
			// ByteCount:   0--2---------4---------6---------8
			// BufferIndex: 0--2---4-----6---8------10--12---14

			size_t bufferIndex = 0;
			while( (bufferIndex < numBytes) && (available()) )
			{
				if(interpolate)
				{
					int32_t sampleA = *((int16_t *)(dataPtr+byteCount-2));
					int32_t sampleB = *((int16_t *)(dataPtr+byteCount));
					int32_t sampleInterp = (sampleA + sampleB) / 2;
					*((int16_t *)(buffer+bufferIndex)) = (int16_t)sampleInterp;
					bufferIndex += 2;  // Advance destination point by 2 bytes (1 sample)
					interpolate = false;
				}
				else
				{
					// Since interpolate is initialized false when opened (when byteCount = 0), we always start here
					// byteCount will then be >=2 when we first run the interpolate section above
					*((int16_t *)(buffer+bufferIndex)) = *((int16_t *)(dataPtr+byteCount));
					byteCount += 2;  // Advance source pointer by 2 bytes (1 sample)
					bufferIndex += 2;  // Advance destination point by 2 bytes (1 sample)
					interpolate = true;
				}
			}
			return bufferIndex;
		}
		void close(void)
		{
			return;
		}
};
