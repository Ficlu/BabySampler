# Compiler and flags
CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -lole32 -luuid -lwinmm -ldsound -lgdi32

# Directories
SRCDIR = src
OBJDIR = build
BINDIR = bin
INCLUDEDIR = include

# Executable
TARGET = $(BINDIR)/babysampler

# Object files
OBJS = $(OBJDIR)/audio_capture.o $(OBJDIR)/audio_save.o $(OBJDIR)/main.o $(OBJDIR)/gui.o

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

$(OBJDIR)/gui.o: $(SRCDIR)/gui.c $(SRCDIR)/gui.h
	@echo "Compiling gui.c into gui.o"
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) -c $(SRCDIR)/gui.c -o $(OBJDIR)/gui.o

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