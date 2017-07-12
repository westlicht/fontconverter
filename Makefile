all: fontconvert

CXX    = g++
CXXFLAGS = -std=c++11 -Wall -I/opt/local/include/freetype2 -I/usr/local/include/freetype2 -I/usr/include/freetype2 -I/usr/include
LIBS   = -L/opt/local/lib -lfreetype

fontconvert: fontconvert.cpp
	$(CXX) $(CXXFLAGS) $< $(LIBS) -o $@
	strip $@

clean:
	rm -f fontconvert
