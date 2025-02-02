import sys
with open(sys.argv[1], 'rb') as file:
  data = file.read()
  index = data.find(b'data')
  print(index + 8)
  file.close()
