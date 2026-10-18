all: twmailer-server

twmailer-server: src/server.cpp
	g++ -o twmailer-server src/server.cpp

clean:
	rm -f twmailer-server
.PHONY: clean
