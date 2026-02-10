# Compiler and flags
CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -lole32 -luuid -lwinmm -ldsound -lgdi32 -lcomctl32

# Directories
SRCDIR = src
OBJDIR = build
BINDIR = bin
INCLUDEDIR = include

# Executable
TARGET = $(BINDIR)/babysampler

# Object files
OBJS = $(OBJDIR)/audio_capture.o $(OBJDIR)/audio_save.o $(OBJDIR)/main.o $(OBJDIR)/recording_list.o $(OBJDIR)/gui.o $(OBJDIR)/pitch_detect.o $(OBJDIR)/scale_detect.o $(OBJDIR)/chromagram.o 

# Default rule to build everything
all: $(TARGET)

# Rule to link the program
$(TARGET): $(OBJS) | $(BINDIR)
	@echo "Linking..."
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Compile each object file independently
$(OBJDIR)/audio_capture.o: $(SRCDIR)/audio_capture.c $(SRCDIR)/audio_capture.h
	@echo "Compiling audio_capture.c into audio_capture.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/audio_capture.c -o $(OBJDIR)/audio_capture.o

$(OBJDIR)/audio_save.o: $(SRCDIR)/audio_save.c $(SRCDIR)/audio_save.h
	@echo "Compiling audio_save.c into audio_save.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/audio_save.c -o $(OBJDIR)/audio_save.o

$(OBJDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/audio_capture.h $(SRCDIR)/audio_save.h $(SRCDIR)/gui.h
	@echo "Compiling main.c into main.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/main.c -o $(OBJDIR)/main.o

$(OBJDIR)/recording_list.o: $(SRCDIR)/recording_list.c $(SRCDIR)/recording_list.h
	@echo "Compiling recording_list.c into recording_list.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/recording_list.c -o $(OBJDIR)/recording_list.o

$(OBJDIR)/gui.o: $(SRCDIR)/gui.c $(SRCDIR)/gui.h
	@echo "Compiling gui.c into gui.o"

	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/gui.c -o $(OBJDIR)/gui.o
$(OBJDIR)/pitch_detect.o: $(SRCDIR)/pitch_detect.c $(SRCDIR)/pitch_detect.h
	@echo "Compiling pitch_detect.c into pitch_detect.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/pitch_detect.c -o $(OBJDIR)/pitch_detect.o

$(OBJDIR)/scale_detect.o: $(SRCDIR)/scale_detect.c $(SRCDIR)/scale_detect.h
	@echo "Compiling scale_detect.c into scale_detect.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/scale_detect.c -o $(OBJDIR)/scale_detect.o

$(OBJDIR)/chromagram.o: $(SRCDIR)/chromagram.c $(SRCDIR)/chromagram.h
	@echo "Compiling chromagram.c into chromagram.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/chromagram.c -o $(OBJDIR)/chromagram.o


# Create the necessary directories
$(OBJDIR):
	@echo "Creating $(OBJDIR) directory"
	mkdir $(OBJDIR)

$(BINDIR):
	@echo "Creating $(BINDIR) directory"
	mkdir $(BINDIR)

# Clean up build files
.PHONY: clean
clean:
	rm -f $(OBJDIR)/*.o $(TARGET)