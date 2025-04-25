#include <stdio.h>
int main()
{
    FILE *fp = fopen("myfile", "w");
    if (!fp)
    {
        printf("fopen error!\n");
    }
    while (1)
        ;
    fclose(fp);
    return 0;
}