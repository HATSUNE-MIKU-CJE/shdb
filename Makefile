CC     = gcc
CFLAGS = -Wall -Wextra
OBJS   = main.o parser.o save.o swap.o read.o 

shdb: $(OBJS)
		$(CC) -o shdb $(OBJS)

main.o: main.c shdb.h
		$(CC) $(CFLAGS) -c $<

parser.o: parser.c shdb.h
		$(CC) $(CFLAGS) -c $<

save.o: save.c shdb.h
		$(CC) $(CFLAGS) -c $<

swap.o: swap.c shdb.h
		$(CC) $(CFLAGS) -c $<

read.o: read.c shdb.h
		$(CC) $(CFLAGS) -c $<

.PHONY: clean
clean:
		rm -f shdb $(OBJS)