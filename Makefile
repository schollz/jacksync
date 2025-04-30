build:
	g++ -o jack_sync main.cpp -ljack -llo -lsndfile -lm -pthread

clean:
	rm -f jack_sync
	rm -f *.o
	rm -f *.so
	rm -f *.a
	rm -f *.dSYM
	rm -f *.swp
	rm -f *.swo
	rm -f *.bak
	rm -f *.orig
	rm -f *.tmp
	rm -rf .DS_Store