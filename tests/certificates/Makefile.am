EXTRA_DIST = \
	certificate-authority.cer \
	server-cert.cer

# Not part of the build, but so we can easily repeat the process
# of generating the testing certificates
rebuild-certs:
	openssl req -new -x509 -out certificate-authority.pem \
		-newkey rsa -keyout certificate-authority.key -nodes \
		-subj /CN=test-ca.empathy.gnome.org -days 2000 \
		-config cert-openssl.conf -set_serial 1 -extensions test_ca
	openssl x509 -in certificate-authority.pem -outform DER \
		-out certificate-authority.cer
	openssl req -new -outform PEM -out server-cert.req \
		-newkey rsa -keyout /dev/null -nodes \
		-subj /CN=test-server.empathy.gnome.org \
		-config cert-openssl.conf -extensions test_server
	openssl x509 -req -in server-cert.req -days 2000 \
		-CA certificate-authority.pem -CAkey certificate-authority.key \
		-set_serial 888 -out server-cert.cer -outform DER
	rm certificate-authority.key
	rm server-cert.req
