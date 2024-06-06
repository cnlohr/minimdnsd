all : minimdnsd

PACKAGE_VERSION:=0.1-$(shell cat .github/build_number)
PACKAGE:=minimdnsd_$(PACKAGE_VERSION)

SHELL=/bin/bash

CFLAGS:=-Wall -pedantic -Os -g -flto -ffunction-sections -Wl,--gc-sections -fdata-sections

minimdnsd : minimdnsd.c
	echo $(shell expr 1 + $(shell cat .github/build_number)) > .github/build_number
	gcc -o $@ $^ $(CFLAGS)
	size $@

install : minimdnsd
	sudo install minimdnsd /usr/local/bin/
	sudo cp minimdnsd.service /etc/systemd/system
	sudo systemctl daemon-reload
	sudo systemctl enable minimdnsd.service
	sudo service minimdnsd restart
	cat minimdnsd.1 | gzip > /usr/share/man/man1/minimdnsd.1.gz

test : minimdnsd
	./minimdnsd &
	./minimdnsd -h testminimdnsd &
	ping -c 1 $(shell cat /etc/hostname).local # Ok, doesn't actually test anything
	killall minimdnsd

deb : minimdnsd
	rm -rf $(PACKAGE)
	mkdir -p $(PACKAGE)/DEBIAN

	mkdir -p $(PACKAGE)/usr/bin
	cp minimdnsd $(PACKAGE)/usr/bin/
	mkdir -p $(PACKAGE)/etc/systemd/system
	cp minimdnsd $(PACKAGE)/etc/systemd/system/
	mkdir -p $(PACKAGE)/usr/share/man/man1
	cat minimdnsd.1 | gzip > $(PACKAGE)/usr/share/man/man1/minimdnsd.1.gz

	cd $(PACKAGE); find . -type f -exec md5sum {} + | cut -c 1-33,38- > DEBIAN/md5sums

	echo "Package: minimdnsd" > $(PACKAGE)/DEBIAN/control
	echo "Version: $(PACKAGE_VERSION)" >> $(PACKAGE)/DEBIAN/control
	echo "Section: base" >> $(PACKAGE)/DEBIAN/control
	echo "Priority: optional" >> $(PACKAGE)/DEBIAN/control
	echo "Architecture: $(shell dpkg --print-architecture)" >> $(PACKAGE)/DEBIAN/control
	echo "Maintainer: cnlohr <lohr85@gmail.com>" >> $(PACKAGE)/DEBIAN/control
	echo "Description: Bare bones MDNS server" >> $(PACKAGE)/DEBIAN/control
	echo "#!/bin/sh" > $(PACKAGE)/DEBIAN/postinst
	echo "set -e" >> $(PACKAGE)/DEBIAN/postinst
	echo 'case "$$1" in' >> $(PACKAGE)/DEBIAN/postinst
	echo "  abort-upgrade|abort-remove|abort-deconfigure|configure)" >> $(PACKAGE)/DEBIAN/postinst
	echo "    systemctl daemon-reload;systemctl enable minimdnsd.service; service minimdnsd restart" >> $(PACKAGE)/DEBIAN/postinst
	echo "    ;;" >> $(PACKAGE)/DEBIAN/postinst
	echo "  triggered)" >> $(PACKAGE)/DEBIAN/postinst
	echo "    systemctl daemon-reload; service minimdnsd restart" >> $(PACKAGE)/DEBIAN/postinst
	echo "    ;;" >> $(PACKAGE)/DEBIAN/postinst
	echo "  *)" >> $(PACKAGE)/DEBIAN/postinst
	echo "    ;;" >> $(PACKAGE)/DEBIAN/postinst
	echo "esac" >> $(PACKAGE)/DEBIAN/postinst
	echo "exit 0" >> $(PACKAGE)/DEBIAN/postinst
	chmod 775 $(PACKAGE)/DEBIAN/postinst

	echo "#!/bin/sh" > $(PACKAGE)/DEBIAN/prerm
	echo "set -e" >> $(PACKAGE)/DEBIAN/prerm
	echo 'case "$$1" in' >> $(PACKAGE)/DEBIAN/prerm
	echo "  remove|remove-in-favour|deconfigure|deconfigure-in-favour)" >> $(PACKAGE)/DEBIAN/prerm
	echo "    systemctl daemon-reload;systemctl disable minimdnsd.service; service minimdnsd stop" >> $(PACKAGE)/DEBIAN/prerm
	echo "    ;;" >> $(PACKAGE)/DEBIAN/prerm
	echo "  upgrade|failed-upgrade)" >> $(PACKAGE)/DEBIAN/prerm
	echo "    service minimdnsd stop" >> $(PACKAGE)/DEBIAN/prerm
	echo "    ;;" >> $(PACKAGE)/DEBIAN/prerm
	echo "  *)" >> $(PACKAGE)/DEBIAN/prerm
	echo "    ;;" >> $(PACKAGE)/DEBIAN/prerm
	echo "esac" >> $(PACKAGE)/DEBIAN/prerm
	echo "exit 0" >> $(PACKAGE)/DEBIAN/postinst
	chmod 775 $(PACKAGE)/DEBIAN/prerm

	dpkg-deb --build $(PACKAGE)


	# For manual installation override
	#mkdir -p $(PACKAGE)/etc/systemd/system/multi-user.target.wants/
	#cd $(PACKAGE)/etc/systemd/system/multi-user.target.wants && ln -s ../minimdnsd.service . || true

clean :
	rm -rf minimdnsd minimdnsd_* -rf
