#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

using namespace std;

const int basesize = 1024;
const int argvnum = 64;
const int envnum = 64;
// 全局的命令行参数表
char *gargv[argvnum];
int gargc = 0;

// 全局的变量
int lastcode = 0;

// 我的系统的环境变量
char *genv[envnum];

// 全局的当前shell工作路径
char pwd[basesize];
char pwdenv[basesize];

// 全局变量与重定向有关
#define NoneRedir 0
#define InputRedir 1
#define OutputRedir 2
#define AppRedir 3

int redir = NoneRedir;
char *filename = nullptr;

// "    "file.txt
#define TrimSpace(pos)        \
    do                        \
    {                         \
        while (isspace(*pos)) \
        {                     \
            pos++;            \
        }                     \
    } while (0)

string GetUserName()
{
    string name = getenv("USER");
    return name.empty() ? "None" : name;
}

string GetHostName()
{
    string hostname = getenv("HOSTNAME");
    return hostname.empty() ? "None" : hostname;
}

string GetPwd()
{
    if (nullptr == getcwd(pwd, sizeof(pwd)))
        return "None";
    snprintf(pwdenv, sizeof(pwdenv), "PWD=%s", pwd);
    putenv(pwdenv); // PWD=XXX
    return pwd;

    // string pwd = getenv("PWD");
    // return pwd.empty() ? "None" : pwd;
}

string LastDir()
{
    string curr = GetPwd();
    if (curr == "/" || curr == "None")
        return curr;
    // /home/whb/XXX
    size_t pos = curr.rfind("/");
    if (pos == std::string::npos)
        return curr;
    return curr.substr(pos + 1);
}

string MakeCommandLine()
{
    // [whb@bite-alicloud myshell]$
    char command_line[basesize];
    snprintf(command_line, basesize, "[%s@%s %s]# ",
             GetUserName().c_str(), GetHostName().c_str(), LastDir().c_str());
    return command_line;
}

void PrintCommandLine() // 1. 命令行提示符
{
    printf("%s", MakeCommandLine().c_str());
    fflush(stdout);
}

bool GetCommandLine(char command_buffer[], int size) // 2. 获取用户命令
{
    // 我们认为：我们要将用户输入的命令行，当做一个完整的字符串
    // "ls -a -l -n"
    char *result = fgets(command_buffer, size, stdin);
    if (!result)
    {
        return false;
    }
    command_buffer[strlen(command_buffer) - 1] = 0;
    if (strlen(command_buffer) == 0)
        return false;
    return true;
}

void ResetCommandline()
{
    memset(gargv, 0, sizeof(gargv));
    gargc = 0;

    // 重定向
    redir = NoneRedir;
    filename = nullptr;
}

void ParseRedir(char command_buffer[], int len)
{
    int end = len - 1;
    while (end >= 0)
    {
        if (command_buffer[end] == '<')
        {
            redir = InputRedir;
            command_buffer[end] = 0;
            filename = &command_buffer[end] + 1;
            TrimSpace(filename);
            break;
        }
        else if (command_buffer[end] == '>')
        {
            if (command_buffer[end - 1] == '>')
            {
                redir = AppRedir;
                command_buffer[end] = 0;
                command_buffer[end - 1] = 0;
                filename = &command_buffer[end] + 1;
                TrimSpace(filename);
                break;
            }
            else
            {
                redir = OutputRedir;
                command_buffer[end] = 0;
                filename = &command_buffer[end] + 1;
                TrimSpace(filename);
                break;
            }
        }
        else
        {
            end--;
        }
    }
}

void ParseCommand(char command_buffer[])
{
    // "ls -a -l -n"
    const char *sep = " ";
    gargv[gargc++] = strtok(command_buffer, sep);
    // =是刻意写的
    while ((bool)(gargv[gargc++] = strtok(nullptr, sep)))
        ;
    gargc--;
}

void ParseCommandLine(char command_buffer[], int len) // 3. 分析命令
{
    ResetCommandline();
    ParseRedir(command_buffer, len);
    ParseCommand(command_buffer);
    // printf("command start: %s\n", command_buffer);
    //  "ls -a -l -n"
    //  "ls -a -l -n" > file.txt
    //  "ls -a -l -n" < file.txt
    //  "ls -a -l -n" >> file.txt

    // printf("redir: %d\n", redir);
    // printf("filename: %s\n", filename);
    // printf("command end: %s\n", command_buffer);
}

void debug()
{
    printf("argc: %d\n", gargc);
    for (int i = 0; gargv[i]; i++)
    {
        printf("argv[%d]: %s\n", i, gargv[i]);
    }
}

// enum
//{
//     FILE_NOT_EXISTS = 1,
//     OPEN_FILE_ERROR,
// };

void DoRedir()
{
    // 1. 重定向应该让子进程自己做！
    // 2. 程序替换会不会影响重定向?不会
    // 0. 先判断 && 重定向
    if (redir == InputRedir)
    {
        if (filename)
        {
            int fd = open(filename, O_RDONLY);
            if (fd < 0)
            {
                exit(2);
            }
            dup2(fd, 0);
        }
        else
        {
            exit(1);
        }
    }
    else if (redir == OutputRedir)
    {
        if (filename)
        {
            int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
            if (fd < 0)
            {
                exit(4);
            }
            dup2(fd, 1);
        }
        else
        {
            exit(3);
        }
    }
    else if (redir == AppRedir)
    {
        if (filename)
        {
            int fd = open(filename, O_CREAT | O_WRONLY | O_APPEND, 0666);
            if (fd < 0)
            {
                exit(6);
            }
            dup2(fd, 1);
        }
        else
        {
            exit(5);
        }
    }
    else
    {
        // 没有重定向,Do Nothong!
    }
}

// 在shell中
// 有些命令，必须由子进程来执行
// 有些命令，不能由子进程执行，要由shell自己执行 --- 内建命令 built command
bool ExecuteCommand() // 4. 执行命令
{
    // 让子进程进行执行
    pid_t id = fork();
    if (id < 0)
        return false;
    if (id == 0)
    {
        // 子进程
        DoRedir();
        // 1. 执行命令
        execvpe(gargv[0], gargv, genv);
        // 2. 退出
        exit(7);
    }
    int status = 0;
    pid_t rid = waitpid(id, &status, 0);
    if (rid > 0)
    {
        if (WIFEXITED(status))
        {
            lastcode = WEXITSTATUS(status);
        }
        else
        {
            lastcode = 100;
        }
        return true;
    }
    return false;
}

void AddEnv(const char *item)
{
    int index = 0;
    while (genv[index])
    {
        index++;
    }

    genv[index] = (char *)malloc(strlen(item) + 1);
    strncpy(genv[index], item, strlen(item) + 1);
    genv[++index] = nullptr;
}
// shell自己执行命令，本质是shell调用自己的函数
bool CheckAndExecBuiltCommand()
{
    if (strcmp(gargv[0], "cd") == 0)
    {
        // 内建命令
        if (gargc == 2)
        {
            chdir(gargv[1]);
            lastcode = 0;
        }
        else
        {
            lastcode = 1;
        }
        return true;
    }
    else if (strcmp(gargv[0], "export") == 0)
    {
        // export也是内建命令
        if (gargc == 2)
        {
            AddEnv(gargv[1]);
            lastcode = 0;
        }
        else
        {
            lastcode = 2;
        }
        return true;
    }
    else if (strcmp(gargv[0], "env") == 0)
    {
        for (int i = 0; genv[i]; i++)
        {
            printf("%s\n", genv[i]);
        }
        lastcode = 0;
        return true;
    }
    else if (strcmp(gargv[0], "echo") == 0)
    {
        if (gargc == 2)
        {
            // echo $?
            // echo $PATH
            // echo hello
            if (gargv[1][0] == '$')
            {
                if (gargv[1][1] == '?')
                {
                    printf("%d\n", lastcode);
                    lastcode = 0;
                }
            }
            else
            {
                printf("%s\n", gargv[1]);
                lastcode = 0;
            }
        }
        else
        {
            lastcode = 3;
        }
        return true;
    }
    return false;
}

// 作为一个shell，获取环境变量应该从系统的配置来
// 我们今天就直接从父shell中获取环境变量
void InitEnv()
{
    extern char **environ;
    int index = 0;
    while (environ[index])
    {
        genv[index] = (char *)malloc(strlen(environ[index]) + 1);
        strncpy(genv[index], environ[index], strlen(environ[index]) + 1);
        index++;
    }
    genv[index] = nullptr;
}

int main()
{
    InitEnv();
    char command_buffer[basesize];
    while (true)
    {
        PrintCommandLine(); // 1. 命令行提示符
        // command_buffer -> output
        if (!GetCommandLine(command_buffer, basesize)) // 2. 获取用户命令
        {
            continue;
        }
        // printf("%s\n", command_buffer);
        // ls
        //"ls -a -b -c -d"->"ls" "-a" "-b" "-c" "-d"
        //"ls -a -b -c -d">hello.txt
        //"ls -a -b -c -d">>hello.txt
        //"ls -a -b -c -d"<hello.txt
        ParseCommandLine(command_buffer, strlen(command_buffer)); // 3. 分析命令

        if (CheckAndExecBuiltCommand())
        {
            continue;
        }

        ExecuteCommand(); // 4. 执行命令
    }
    return 0;
}