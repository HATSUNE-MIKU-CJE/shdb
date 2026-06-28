CC     = gcc
CFLAGS = -Wall -Wextra
OBJS   = main.o parser.o storage.o swap.o query.o crc32.o

shdb: $(OBJS)
		$(CC) -o shdb $(OBJS)

main.o: main.c shdb.h
		$(CC) $(CFLAGS) -c $<

parser.o: parser.c shdb.h
		$(CC) $(CFLAGS) -c $<

storage.o: storage.c shdb.h
		$(CC) $(CFLAGS) -c $<

swap.o: swap.c shdb.h
		$(CC) $(CFLAGS) -c $<

query.o: query.c shdb.h
		$(CC) $(CFLAGS) -c $<
		
crc32.o: crc32.c shdb.h
		$(CC) $(CFLAGS) -c $<

.PHONY: clean
clean:
		rm -f shdb $(OBJS)