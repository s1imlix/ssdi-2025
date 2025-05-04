ARCH := arm64
USER_HEADERS := $(PWD)
KERNEL_HEADERS := $(PWD)/../linux/linux-headers/include
CROSS := aarch64-linux-gnu-
EXTRA_CFLAGS := -Wall -g -I$(KERNEL_HEADERS) -I$(USER_HEADERS)

TARGET_1 := userfaultfd 
TARGET_2 := test 

SRC_1 := userfaultfd.c 
SRC_2 := test.c 
SRC_COMMON := helper.c 

OBJ_1 := $(SRC_1:.c=.o) $(SRC_COMMON:.c=.o) 
OBJ_2 := $(SRC_2:.c=.o) $(SRC_COMMON:.c=.o) 

all: $(TARGET_1) $(TARGET_2) 

$(TARGET_1): $(OBJ_1)
	$(CROSS)gcc -o $@ $^ $(EXTRA_CFLAGS) 

$(TARGET_2): $(OBJ_2) 
	$(CROSS)gcc -o $@ $^ $(EXTRA_CFLAGS)

%.o: %.c 
	$(CROSS)gcc -c $< $(EXTRA_CFLAGS) 

clean: 
	rm -f $(TARGET_1) $(TARGET_2) $(OBJ_1) $(OBJ_2) 
