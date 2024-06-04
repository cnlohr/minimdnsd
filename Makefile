all : minimdnsd

GITHASH:=$(shell git describe --abbrev=0 --tags || git rev-parse HEAD)

minimdnsd : minimdnsd.c
	gcc -o $@ $^ -Os -g
	size $@

install : minimdnsd
	sudo install minimdnsd /usr/local/bin/
	sudo cp minimdnsd.service /etc/systemd/system
	sudo systemctl daemon-reload
	sudo systemctl enable minimdnsd.service
	sudo service minimdnsd restart

deb :
	echo $(GITHASH)

clean :
	rm -rf minimdnsd
