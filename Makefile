all : minimdnsd

PACKAGE_VERSION:=0.1-$(shell cat .github/build_number)
PACKAGE:=minimdnsd_$(PACKAGE_VERSION)

minimdnsd : minimdnsd.c
	echo $(shell expr 1 + $(shell cat .github/build_number)) > .github/build_number
	gcc -o $@ $^ -Os -g
	size $@

install : minimdnsd
	sudo install minimdnsd /usr/local/bin/
	sudo cp minimdnsd.service /etc/systemd/system
	sudo systemctl daemon-reload
	sudo systemctl enable minimdnsd.service
	sudo service minimdnsd restart

test : minimdnsd
	./minimdnsd &
	ping -c 1 $(shell cat /etc/hostname).local
	killall minimdnsd

deb : minimdnsd
	mkdir -p $(PACKAGE)/DEBIAN
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

	mkdir -p $(PACKAGE)/user/bin
	cp minimdnsd $(PACKAGE)/user/bin/
	mkdir -p $(PACKAGE)/etc/systemd/system
	cp minimdnsd $(PACKAGE)/etc/systemd/system/
	dpkg-deb --build $(PACKAGE)


	# For manual installation override
	#mkdir -p $(PACKAGE)/etc/systemd/system/multi-user.target.wants/
	#cd $(PACKAGE)/etc/systemd/system/multi-user.target.wants && ln -s ../minimdnsd.service . || true

clean :
	rm -rf minimdnsd minimdnsd_* -rf
