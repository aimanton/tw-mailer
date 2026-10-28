all: twmailer-server twmailer-client

twmailer-server: src/server.cpp
	g++ -std=c++17 -Wall -o twmailer-server src/server.cpp

twmailer-client: src/client.cpp
	g++ -std=c++17 -Wall -o twmailer-client src/client.cpp

clean:
	rm -f twmailer-server twmailer-client

.PHONY: all clean
