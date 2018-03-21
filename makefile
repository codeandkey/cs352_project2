CC = gcc
CFLAGS = -std=gnu99 -Wall -Werror -O3
LDFLAGS =

OUTPUT_CL = client
OUTPUT_SV = server

SOURCES_CL = $(wildcard src/client/*.c)
OBJECTS_CL = $(SOURCES_CL:.c=.o)

SOURCES_SV = $(wildcard src/server/*.c)
OBJECTS_SV = $(SOURCES_SV:.c=.o)

all: $(OUTPUT_CL) $(OUTPUT_SV)

$(OUTPUT_CL): $(OBJECTS_CL)
	@echo ld $(OUTPUT_CL)
	@$(CC) $(OBJECTS_CL) $(LDFLAGS) -o $(OUTPUT_CL)

$(OUTPUT_SV): $(OBJECTS_SV)
	@echo ld $(OUTPUT_SV)
	@$(CC) $(OBJECTS_SV) $(LDFLAGS) -o $(OUTPUT_SV)

%.o: %.c
	@echo cc $<
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -f $(OBJECTS_SV) $(OBJECTS_CL) $(OUTPUT_SV) $(OUTPUT_CL)
