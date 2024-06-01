all : minimdnsd

minimdnsd : minimdnsd.c
	gcc -o $@ $^ -O2 -g

clean :
	rm -rf minimdnsd
