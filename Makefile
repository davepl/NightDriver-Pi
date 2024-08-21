# NDPi Makefile davepl Aug-15-2024
#
# -MMD: This option tells gcc or g++ to generate a .d file containing the dependencies for each .cpp or .c file.
# 	    The .d file will list the source file and all the headers it depends on.
# -MP:  This flag adds phony targets for each header file to avoid issues if the header is deleted. This prevents
#       Make from throwing an error when it tries to rebuild based on a deleted header.

CFLAGS=-Wall -Ofast -g -Wextra -Wno-unused-parameter -MMD -MP
CXXFLAGS=$(CFLAGS)
OBJECTS=main.o
BINARIES=ndpi

# Where our library resides. You mostly only need to change the
# RGB_LIB_DISTRIBUTION, this is where the library is checked out.

RGB_LIB_DISTRIBUTION=rpi-rgb-led-matrix
RGB_INCDIR=$(RGB_LIB_DISTRIBUTION)/include
RGB_LIBDIR=$(RGB_LIB_DISTRIBUTION)/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a
LDFLAGS+=-L$(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -lrt -lm -lpthread -lstdc++ -lz

all : $(BINARIES)

ndpi : $(OBJECTS) $(RGB_LIBRARY)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(RGB_LIBRARY) :
	$(MAKE) -C $(RGB_LIB_DISTRIBUTION)

%.o : %.cpp
	$(CXX) -I$(RGB_INCDIR) $(CXXFLAGS) -c -o $@ $<

%.o : %.c
	$(CC) -I$(RGB_INCDIR) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(BINARIES) $(OBJECTS:.o=.d)


-include $(OBJECTS:.o=.d)

FORCE:
.PHONY: FORCE