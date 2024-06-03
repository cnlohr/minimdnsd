all : minimdnsd

minimdnsd : minimdnsd.c
	gcc -o $@ $^ -Os -g
	size $@

clean :
	rm -rf minimdnsd
