#include <stdio.h>
#include <string.h>
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("argc error!\n");
        return 1;
    }
    FILE *fp = fopen(argv[1], "r");
    if (!fp)
    {
        printf("fopen error!\n");
        return 2;
    }
    char buf[1024];
    while (1)
    {
        size_t s = fread(buf, 1, sizeof(buf), fp);
        if (s > 0)
        {
            buf[s] = 0;
            printf("%s", buf);
        }
        if (feof(fp))
        {
            break;
        }
    }
    fclose(fp);
    return 0;
}