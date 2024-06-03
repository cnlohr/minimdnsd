all : minimdnsd

minimdnsd : minimdnsd.c
	gcc -o $@ $^ -Os -g
	size $@

install : minimdnsd
	install minimdnsd /usr/local/bin/

clean :
	rm -rf minimdnsd
