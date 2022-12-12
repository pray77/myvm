#include <stdio.h>
#include <stdint.h>
#include <signal.h>
/* unix only */
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>


#define MEMORY_MAX (1 << 16)
uint16_t memory[MEMORY_MAX]; //vm的内存

enum
{
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC, /* 程序计数器 */
    R_COND, /* 标志位寄存器 */
    R_COUNT /* 寄存器数量 */
};

enum
{
    FL_POS = 1 << 0, /* p 正*/
    FL_ZRO = 1 << 1, /* Z 零 */
    FL_NEG = 1 << 2, /* N 负数 */
};

uint16_t reg[R_COUNT];

enum
{
    OP_BR = 0,  /* branch 根据标志位跳转 */
    OP_ADD,     /* add 加法 */
    OP_LD,      /* load 加载 读 */
    OP_ST,      /* store 存储 写 */
    OP_JSR,     /* Jump to Subroutine 类似 CALL */
    OP_AND,     /* bitwise and 按位与 */
    OP_LDR,     /* load register 从寄存器加载 */
    OP_STR,     /* store register 写寄存器 */
    OP_RTI,     /* unused 未使用的 */
    OP_NOT,     /* bitwise not 按位取反 */
    OP_LDI,     /* load indirect 间接加载 */
    OP_STI,     /* store indirect 间接存储 */
    OP_JMP,     /* jump 跳转 */
    OP_RES,     /* reserved (unused) 保留 */
    OP_LEA,     /* load effective address 加载一个有效地址*/
    OP_TRAP     /* execute trap 执行捕捉 */
};

enum
{
    TRAP_GETC = 0x20,
    TRAP_OUT,
    TRAP_PUTS,
    TRAP_IN,
    TRAP_PUTSP,
    TRAP_HALT
};

enum
{
    MR_KBSR = 0xFE00,
    MR_KBDR = 0xFE02
};

struct termios original_tio;

void disable_input_buffering()
{
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t check_key()
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

void mem_write(uint16_t address, uint16_t val)
{
    memory[address] = val;
}

uint16_t mem_read(uint16_t address)
{
    if (address == MR_KBSR)
    {
        if (check_key())
        {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else
        {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[address];
}

uint16_t sign_extend(uint16_t x, int bit_count)
{
    if ((x >> (bit_count -1)) & 1) {
        x |= (0xffff << bit_count);
    }
    return x;
}

void update_flags(uint16_t r)
{
    if (reg[r] == 0)
    {
        reg[R_COND] = FL_ZRO;
    }
    else if (reg[r] >> 15) //最高位存储符号位
    {
        reg[R_COND] = FL_NEG;
    }
    else
    {
        reg[R_COND] = FL_POS;
    }
    
}

uint16_t swap16(uint16_t x)
{
    return (x << 8) | (x >> 8);
}

void read_image_file(FILE* file)
{
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    while (read-- > 0)
    {
        *p = swap16(*p);
        ++p;
    }
    
}

int read_image(const char* image_path)
{
    FILE* file = fopen(image_path, "rb");
    if (!file) return 0;
    read_image_file(file);
    fclose(file);
    return 1;
}


int main(int argc, const char* argv[])
{
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();
    if (argc < 2)
    {
        printf("lc3 [image-files] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j)
    {
        if (!read_image(argv[j]))
        {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }
    reg[R_COND] = FL_ZRO;

    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;
    while (running)
    {
        uint16_t instr = mem_read(reg[R_PC]++);
        uint16_t op = instr >> 12; //每条指令16位，最后四位存放指令

        switch (op)
        {
            case OP_ADD:  //加法
            {
                uint16_t r0 = (instr >> 9) & 0x7; // 目的寄存器
                uint16_t r1 = (instr >> 6) & 0x7; // 操作寄存器1
                uint16_t imm_flag = (instr >> 5) &1; // 立即数模式flag位

                if (imm_flag)
                {
                    uint16_t imm5 = sign_extend(instr & 0x1f, 5);
                    reg[r0] = reg[r1] + imm5;
                }
                else
                {
                    uint16_t r2 = instr & 0x7; //操作寄存器2
                    reg[r0] = reg[r1] + reg[r2];
                }

                update_flags(r0);
            }
                break;
            case OP_AND:  //按位与
            {
                uint16_t r0 = (instr >> 9) & 0x7; // 目的寄存器
                uint16_t r1 = (instr >> 6) & 0x7; // 操作寄存器1
                uint16_t imm_flag = (instr >> 5) & 0x1; // 立即数模式flag位

                if (imm_flag)
                {
                    uint16_t imm5 = sign_extend(instr & 0x1f, 5);
                    reg[r0] = reg[r1] & imm5;  //立即数按位与
                }
                else
                {
                    uint16_t r2 = instr & 0x7;
                    reg[r0] = reg[r1] & reg[r2];  //寄存器按位与
                }

                update_flags(r0);
            }
                break;
            case OP_NOT: //对寄存器取反
            {
                uint16_t r0 = (instr >> 9) & 0x7; // 目的寄存器
                uint16_t r1 = (instr >> 6) & 0x7; // 操作寄存器1

                reg[r0] = ~reg[r1];

                update_flags(r0);
            }
                break;
            case OP_BR: //按标志位跳转
            {
                uint16_t cond_flag = (instr >> 9 ) & 0x7; // 正数标志位
                uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);

                if (cond_flag & reg[R_COND])
                    reg[R_PC] += pc_offset;
            }
                break;
            case OP_JMP:  //跳转到寄存器指向的值
            {
                uint16_t r1 = (instr >> 6) & 0x7; //目的寄存器

                reg[R_PC] = reg[r1];
            }
                break;
            case OP_JSR:  //call [reg] or call [pc + pc_offset]
            {
                uint16_t imm_flag = (instr >> 11) & 0x1; // 立即数标志位

                reg[R_R7] = reg[R_PC];
                if (imm_flag)
                {
                    uint16_t pc_offset = sign_extend(instr & 0x7ff, 11);
                    reg[R_PC] += pc_offset;   // JSR
                }
                else
                {
                    uint16_t r1 = (instr >> 6) & 0x7; // 操作寄存器1
                    reg[R_PC] = reg[r1];    // JSRR
                }
            }
                break;
            case OP_LD:  //取地址存储的值给寄存器（按pc偏移）
            {
                uint16_t r0 = (instr >> 9) & 0x7; // 目的寄存器
                uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);

                reg[r0] = mem_read(reg[R_PC] + pc_offset);
                update_flags(r0);
            }
                break;
            case OP_LDI:  //取地址存储的地址的值给寄存器
            {
                uint16_t r0 = (instr >> 9) & 0x7; //目的寄存器
                uint16_t pc_offset = sign_extend(instr & 0x1ff, 9); //将0-8位拓展到16位

                reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset)); // 按地址取值
                update_flags(r0); //更新标志位
            }
                break;
            case OP_LDR:  //取地址存储的值给寄存器（按寄存器偏移）
            {
                uint16_t r0 = (instr >> 9) & 0x7; //目的寄存器
                uint16_t r1 = (instr >> 6) & 0x7; //操作寄存器1
                uint16_t offset = sign_extend(instr & 0x3f, 6);

                reg[r0] = mem_read(reg[r1] + offset);
                update_flags(r0);
            }
                break;
            case OP_LEA:  // Load Effective Address 取一个有效地址给寄存器（按pc偏移）
            {
                uint16_t r0 = (instr >> 9) & 0x7; //目的寄存器
                uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);

                reg[r0] = reg[R_PC] + pc_offset;
                update_flags(r0);
            }
                break;
            case OP_ST:  // 把寄存器的值存到内存上（按pc偏移）
            {
                uint16_t r1 = (instr >> 9) & 0x7; // 操作寄存器1
                uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);

                mem_write(reg[R_PC] + pc_offset, reg[r1]);
            }
                break;
            case OP_STI:  //把寄存器的值存到内存的一个地址所指向的内存处
            {
                uint16_t r1 = (instr >> 9) & 0x7; // 操作寄存器1
                uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);

                mem_write(mem_read(reg[R_PC] + pc_offset), reg[r1]);
            }
                break;
            case OP_STR:  //把寄存器的值存到内存（按寄存器偏移）
            {
                uint16_t r1 = (instr >> 9) & 0x7; // 操作寄存器1
                uint16_t r2 = (instr >> 6) & 0x7; // 操作寄存器2
                uint16_t offset = sign_extend(instr & 0x3f, 6);

                mem_write(reg[r2] + offset, reg[r1]);
            }
                break;
            case OP_TRAP:  //硬件处理
            {
                reg[R_R7] = reg[R_PC];
                
                switch (instr & 0xff)
                {
                case TRAP_GETC: //输入一个字符给r0寄存器
                    reg[R_R0] = getchar();
                    update_flags(R_R0);
                    break;
                case TRAP_OUT:  //输出r0寄存器的值对应的字符
                    putc((char)reg[R_R0], stdout);
                    fflush(stdout);
                    break;
                case TRAP_PUTS:  //输出内存上的字符串（r0寄存器存偏移），每两字节
                {
                    uint16_t* c = memory + reg[R_R0];
                    while (*c)
                    {
                        putc((char)*c, stdout);
                        ++c;
                    }
                    fflush(stdout);
                }
                    break;
                case TRAP_IN:  //输入一个字符给r0寄存器，同时打印
                {
                    printf("Enter a character: ");
                    char c = getchar();
                    putc(c, stdout);
                    fflush(stdout);
                    reg[R_R0] = (uint16_t)c;
                    update_flags(R_R0);
                }
                    break;
                case TRAP_PUTSP:  //输出内存上的字符串（r0寄存器存偏移），每一字节
                {
                    uint16_t* c = memory + reg[R_R0];
                    while (*c)
                    {
                        char char1 = (*c) & 0xff;
                        putc(char1, stdout);
                        char char2 = (*c) >> 8;
                        if (char2) putc(char2, stdout);
                        ++c;
                    }
                    fflush(stdout);
                }
                    break;
                case TRAP_HALT:
                    puts("HALT");  //停止运行
                    fflush(stdout);
                    running = 0;
                    break;
                default:
                    break;
                }
            }
                break;
            case OP_RES:
            case OP_RTI:
                abort();
            default:
                break;
        }
    }
    restore_input_buffering();
}
