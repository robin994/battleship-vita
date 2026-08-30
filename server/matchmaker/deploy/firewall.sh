#!/bin/sh
# Open the matchmaker port. The board only needs ONE inbound TCP port.
# (Gameplay never touches this machine — it is peer-to-peer between the two
# consoles; the host console's own router must forward 26041/tcp + 26042/udp.)
set -e

PORT=26050

if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
	echo "firewalld detected"
	sudo firewall-cmd --permanent --add-port=${PORT}/tcp
	sudo firewall-cmd --reload
elif command -v ufw >/dev/null 2>&1; then
	echo "ufw detected"
	sudo ufw allow ${PORT}/tcp
else
	echo "using iptables"
	sudo iptables -I INPUT 6 -p tcp --dport ${PORT} -j ACCEPT
	if command -v netfilter-persistent >/dev/null 2>&1; then
		sudo netfilter-persistent save
	elif [ -d /etc/iptables ]; then
		sudo sh -c 'iptables-save > /etc/iptables/rules.v4'
	fi
fi
echo "done. Remember to forward tcp/${PORT} on your router to this machine."
