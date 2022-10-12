all:
	gcc -g -o egl_fake main.c log.c -O2 -ldrm -lEGL -lgbm -lGL -I/usr/include/libdrm 
clean:
	rm egl_fake

