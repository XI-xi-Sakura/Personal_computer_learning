

## Python 背景知识

### 来源
吉多·范罗苏姆（Guido van Rossum) 是一个荷兰程序员(人称龟叔, 名字前三个字母是 Gui),龟叔在 1989 年圣诞节的时候(当时 33 岁), 因为在家里待着无聊, 为了打发时间, 开始了 Python 的开发. 第一个正式版本发布于 1991 年.


### 用途
经历了多年的发展, Python 目前是一个应用场景非常广泛的编程语言. 

```
科学计算&数据分析
Web 开发(搭建网站)
自动化运维
人工智能
爬虫程序
自动化测试

```

### 优缺点
优点:

1. 语法言简意赅, 容易上手. 
2. 功能强大, 用途广泛. 
3. 生态丰富, 具有海量的成熟第三方库.
4. 方便调用 C/C++ 编写的代码进行高性能/系统级操作. 

缺点:
1. 执行效率比较弱. 
6. 对于多核心并发程序支持偏弱. 
3. 动态类型系统对于大型项目不太友好
## 搭建Python 环境

要想能够进行 Python 开发, 就需要搭建好 Python 的环境. 

需要安装的环境主要是两个部分:
1. 运行环境: Python
2. 开发环境: PyCharm
### [安装 Python](https://www.python.org/)

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/00cccec002a14376ab0ef05e70ba62a2.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/2e6809b580824dfbb4ab006d19c114dd.png)
**注意**: Python 的版本在持续迭代更新中. 同学们看到的版本不一定和我这里完全一致, 但是基本不影响学习使用.

```
关于 Python 版本的补充
现在主流使用的 Python 版本是 Python 3 系列. 但是同学们以后在公司中, 接触到一些比较老的项
目, 可能还在使用 Python 2 . 
3 系列 和 2 系列 之间的语法还是存在不小差别的. 学习的内容主要是依据 Python 3 系列展开.
```
下载完成后, 会得到一个 exe 的安装程序。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/48f948c2982b45a6a36844e32273c98f.png)
安装即可。
注意:
1. 最好勾选下 "Add Python 3.10 to PATH" 这个选项. 
2. 要记得 Python 的安装目录(后面可能会用到).


如果直接双击这个 python.exe , 就会打开 Python 的交互式解释器(控制台程序). 在这个交互式解释器中, 就可以输入 Python 代码了. 
直接输入 print('hello') 这句代码, 按下 enter 键, 则打印出了 hello 这样的结果。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/3f0546ff1ec24506a423265a5fa75015.png)
### [安装 PyCharm](https://www.jetbrains.com/pycharm/download/?section=windows#section=windows)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/2eb32867f2774ed6ba19472f06315943.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/6629c986540f42599c975546063653ee.png)
PyCharm针对每个平台都有Professional和Community两个版本，自己可根据自己的需要进行选择。
Professional：专业版（建议选择专业版），功能强大，属于收费版。
Community：社区版，只支持Python开发，开源、免费，用作学习也够用。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/82b80d6c3e234fe3a91101e7aa123d54.png)


![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/7429f96bc19842f1a94b9bfd17908c27.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/4cb816500c414a3db6a0accc35b68eaf.png)
之后直接安装即可。


## 新建项目
一、创建一个项目
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/874606f4cd5f42e2ba99fe6d9c92472e.png)
二、选择项目所在的位置, 并选择使用的 Python 解释器.
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/67b449b63a544e59a5863498a39bad42.png)
注：一般情况下, PyCharm 能够自动识别出 Python 解释器的位置. 但是如果没有自动识别出来, 
也没关系, 点击右侧的 ... 选择之前安装的 Python 的路径位置即可。

三、创建文件
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b219bdfb6f16415b84c53e340f7c0bc0.png)

