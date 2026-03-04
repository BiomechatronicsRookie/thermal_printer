#include<stdio.h>

int main(){
	// Define character to put on stream
	int status = 0;
	char word[] = "This is a story that will be loaded on reading the SD card. I don't know how long the buffer can be, simply put super long sentences and they should be enough";

	// Instruction set [basically print the buffer and  a set of lines]
	int complex_instructions[8] = {0x1B, 0x64, 0x04};
	// Open the stream port on read mode
	const char * restrict path = "/dev/serial0";
 	char mode = 'w';
	FILE * pport = fopen(path, &mode);

	// Write a character or string to the port
	status |= fputs(word, pport);

	for (int i = 0; i < 8; i++){
		status |= fputc(complex_instructions[i], pport);
	}
	// If EOF print error for now
	if(status == EOF){
		printf("Error: Status of the write function %d", status);
	};

	// Close the stream port
	if (fclose(pport)){
		return 0;
	} else{
		return -1;
	};
};
