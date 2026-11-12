server: serverChris.c
	gcc -o server serverChris.c -I.

clean:
	rm -f server