all : minimdnsd

minimdnsd : minimdnsd.c
	gcc -o $@ $^ -Os -g
	size $@

install : minimdnsd
	sudo install minimdnsd /usr/local/bin/
	sudo cp minimdnsd.service /etc/systemd/system
	sudo systemctl enable minimdnsd.service

clean :
	rm -rf minimdnsd
