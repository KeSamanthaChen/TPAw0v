#include<stdio.h>

int main() {
	printf("hello world!\n");
	int i=0;
	for(i=0; i<100;i++) {
		i += i;
	}
	printf("results %d\n", i);
	return 0;
}
