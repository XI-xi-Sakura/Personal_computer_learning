#include <mysql/mysql.h>  
#include <stdio.h>  
  
int main() {  
    // 调用 mysql_get_client_info 函数获取客户端库版本信息  
    const char *client_info = mysql_get_client_info();  
  
    // 打印客户端库版本信息  
    if (client_info) {  
        printf("MySQL client library version: %s\n", client_info);  
    } else {  
        printf("Failed to get MySQL client library version.\n");  
    }  
  
    // 注意：虽然这个示例没有实际连接到数据库，但调用 mysql_get_client_info 不需要连接  
    // 如果你需要进行数据库操作，需要初始化 MySQL 库并连接数据库，这超出了这个简单示例的范围  
  
    return 0;  
}