## 1. 理解硬件

### 1-1 磁盘物理结构
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/bbdc5e936876406b882bd4d241eaebd0.png)
### 1-2 磁盘的存储结构


扇区：是磁盘存储数据的基本单位，512字节，块设备
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/dc278859dafd44b084af2daae99e5a15.jpeg)
如何定位一个扇区呢？

- 可以先定位磁头（header）
- 确定磁头要访问哪一个柱面(磁道)（cylinder）
- 定位一个扇区(sector)

### 1-3 CHS地址定位

文件 = 内容+属性 都是数据，无非就是占据那几个扇区的问题！能定位一个扇区了，能不能定位多个扇区呢?

```
(base) ubuntu@VM-8-17-ubuntu:~$ sudo fdisk -l
Disk /dev/vda: 60 GiB, 64424509440 bytes, 125829120 sectors
Units: sectors of 1 * 512 = 512 bytes
Sector size (logical/physical): 512 bytes / 512 bytes
I/O size (minimum/optimal): 512 bytes / 512 bytes
Disklabel type: gpt
Disk identifier: 884D39AE-2030-4231-B486-520515A9ADD7

Device     Start       End   Sectors Size Type
/dev/vda1   2048      4095      2048   1M BIOS boot
/dev/vda2   4096 125829086 125824991  60G Linux filesystem
```
• 扇区是从磁盘读出和写入信息的最小单位，通常大小为 512 字节。
• 磁头（head）数：每个盘片一般有上下两面，分别对应1个磁头，共2个磁头
• 磁道（track）数：磁道是从盘片外圈往内圈编号0磁道，1磁道...，靠近主轴的同心圆用于停靠磁头，不存储数据
• 柱面（cylinder）数：磁道构成柱面，数量上等同于磁道个数
• 扇区（sector）数：每个磁道都被切分成很多扇形区域，每道的扇区数量相同
• 圆盘（platter）数：就是盘片的数量
• 磁盘容量=磁头数 × 磁道(柱面)数 × 每道扇区数 × 每扇区字节数
• 细节：传动臂上的磁头是**共进退**的(这点比较重要，后面会说明)

柱面（cylinder），磁头（head），扇区（sector），显然可以定位数据了，这就是数据定位(寻址)方式之一，CHS寻址方式。


> ### 1-4 CHS寻址
>对早期的磁盘非常有效，知道用哪个磁头，读取哪个柱面上的第几扇区就可以读到数据了。
>但是CHS模式支持的硬盘容量有限，因为系统用8bit来存储磁头地址，用10bit来存储柱面地址，用6bit来存储扇区地址，而一个扇区共有512Byte，这样使用CHS寻址一块硬盘最大容量为256 * 1024 * 63 * 512B = 8064 MB(1MB = 1048576B)（若按1MB=1000000B来算就是8.4GB） 

### 1-5 磁盘的逻辑结构 
![](https://i-blog.csdnimg.cn/direct/e6fef7903498493287d5010689ec50b3.png)

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/0d3002b7f0d646acaa38c835e239937b.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/adc07a92c29c498c8b48c035a86479e0.png)

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/8b9ddf424f604537848a74273b59dff9.png)
OS只需要使用LBA就可以了！！LBA地址转成CHS地址，CHS如何转换成为LBA地址。谁做啊？？磁盘自己来做！固件(硬件电路，伺服系统)

###  1-6 CHS && LBA地址
**CHS转成LBA**：
- 磁头数*每磁道扇区数 = 单个柱面的扇区总数
- LBA = 柱面号C*单个柱面的扇区总数 + 磁头号H*每磁道扇区数 + 扇区号S - 1
- 即：LBA = 柱面号C*(磁头数*每磁道扇区数) + 磁头号H*每磁道扇区数 + 扇区号S - 1
- 扇区号通常是从1开始的，而在LBA中，地址是从0开始的
- 柱面和磁道都是从0开始编号的
- 总柱面，磁道个数，扇区总数等信息，在磁盘内部会自动维护，上层开机的时候，会获取到这些参数。

**LBA转成CHS**：
- 柱面号C = LBA // (磁头数*每磁道扇区数)【就是单个柱面的扇区总数】 
- 磁头号H = (LBA % (磁头数*每磁道扇区数)) // 每磁道扇区数
- 扇区号S = (LBA % 每磁道扇区数) + 1
- "//":表示除取整

所以：从此往后，在磁盘使用者看来，根本就不关心CHS地址，而是直接使用LBA地址，磁盘内部自己转换。所以：

从现在开始，磁盘就是一个元素为扇区的一维数组，数组的下标就是每一个扇区的LBA地址。OS使用磁盘，就可以用一个数字访问磁盘扇区了。 

### 2. 引入文件系统
#### 2-1 引入“块”概念
其实硬盘是典型的 “块” 设备，**操作系统读取硬盘数据的时候，其实是不会一个扇区地读取，这样效率太低，而是一次性连续读取多个扇区，即一次性读取一个“块”**。

“块” 的大小是由格式化的时候确定的，并且不可以更改，最常见的是4KB，即连续八个扇区组成一个 “块” 。“块” 是文件存取的最小单位。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/cd85b573ab1e4d4f9218cee9b416fbbf.png)

```bash
$ stat main.c
  File: ‘main.c’
  Size: 488            Blocks: 8          IO Block: 4096   regular file
Device: 064h/64769d    Inode: 1052007     Links: 1
Access: (0644/-rw-r--r--)  Uid: ( 1000/    whb)   Gid: ( 1000/    whb)
Access: 2024-10-25 22:15:52.909616443 +0800
Modify: 2024-10-17 19:06:11.139423737 +0800
Change: 2024-10-17 19:06:11.139423737 +0800
Birth: -
```

**注意**：
- 磁盘就是一个三维数组，我们把它看待成为一个“一维数组”，数组下标就是LBA，每个元素都是扇区。
- 每个扇区都有LBA，那么8个扇区一个块，每一个块的地址我们也能算出来。
- 知道LBA：块号 = LBA/8
- 知道块号：LAB=块号*8 + n.(n是块内第几个扇区)

### 2-2 引入“分区”概念
其实磁盘是可以被分成多个分区（partition）的，以Windows观点来看，你可能会有一块磁盘并且将它分成C盘,D盘,E盘，那个C,D,E就是分区。

分区从实质上说就是对**硬盘的一种格式化**。

但是Linux的设备都是以文件形式存在，那是怎么分区的呢？

**柱面是分区的最小单位**，我们可以利用参考柱面号码的方式来进行分区，其本质就是设置每个区的起始柱面和结束柱面号码。此时我们可以将硬盘上的柱面（分区）进行平铺，将其想象成一个大的平面。

**注意**：
- 柱面大小一致，扇区个位一致，那么其实只要知道每个分区的起始和结束柱面号，知道每一个柱面多少个扇区，那么该分区多大，其实和解释LBA是多少也就清楚了。

### 2-3 引入“inode”概念
之前我们说过 **文件=数据+属性** ，我们使用 `ls -l` 的时候看到的除了看到文件名，还能看到文件元数据（属性）。

```bash
]# ls -l
总用量 12
-rwxr-xr-x. 1 root root 7438 "9月  13 14:56" a.out
-rw-r--r--. 1 root root  654 "9月  13 14:56" test.c
```

每行包含7列：
- 模式
- 硬链接数
- 文件所有者
- 组
- 大小
- 最后修改时间
- 文件名

`ls -l`读取存储在磁盘上的文件信息，然后显示出来。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/48f51e0c9dca44c8a2c198558b596965.png)

其实这个信息除了通过这种方式来读取，还有一个`stat`命令能够看到更多信息。

```bash
[root@localhost linux]# stat test.c
  File: ‘test.c’
  Size: 654            Blocks: 8          IO Block: 4096   普通文件
Device: 802h/2050d    Inode: 263715      Links: 1
Access: (0644/-rw-r--r--)  Uid: (  0/    root)   Gid: (  0/    root)
Access: 2017-09-13 14:56:57.059012947 +0800
Modify: 2017-09-13 14:56:40.0679012944 +0800
Change: 2017-09-13 14:56:40.069012948 +0800
```

我们要思考一个问题，文件数据都储存在”块”中，那么很显然，我们还必须找到一个地方储存文件的元信息（属性信息），比如文件的创建者、文件的创建日期、文件的大小等等。这种储存文件元信息的区域就叫做`inode`，中文译名为”**索引节点**”。

```bash
$ ls -li
total 16
1052669 -rw-rw-r-- 1 whb whb  225 Oct 17 19:09
1052669 -rw-rw-r-- 1 whb whb  485 Oct 17 19:09
1052667 -rw-rw-r-- 1 whb whb 1486 Oct 17 19:10
1052661 -rw-rw-r-- 1 whb whb 1496 Oct 17 19:11
1052662 -rw-rw-r-- 1 whb whb  447 Oct 17 18:53
```

每一个文件都有对应的inode，里面包含了与该文件有关的一些信息。为了能解释清楚inode，我们需要是深入了解一下文件系统。

**注意**：
- Linux下文件的**存储是属性和内容分离存储的**。
- Linux下，保存文件属性的集合叫做**inode**，一个文件，一个inode，inode内有一个唯一的标识符，叫做**inode号**。

所以一个文件的属性inode长什么样子呢？

```c
/*
 * Structure of an inode on the disk
 */
struct ext2_inode {
    __le16  i_mode;       /* File mode */
    __le16  i_uid;        /* Low 16 bits of Owner Uid */
    __le32  i_size;       /* Size in bytes */
    __le32  i_atime;      /* Access time */
    __le32  i_ctime;      /* Creation time */
    __le32  i_mtime;      /* Modification time */
    __le32  i_dtime;      /* Deletion Time */
    __le16  i_gid;        /* Low 16 bits of Group Id */
    __le16  i_links_count;/* Links count */
    __le32  i_blocks;     /* Blocks count */
    __le32  i_flags;      /* File flags */
    union {
        struct {
            __le32  l_i_reserved1;
        } linux1;
        struct {
            __le32  h_i_translator;
        } hurd1;
        struct {
            __le32  m_i_reserved1;
        } masix1;
    } osd1;               /* OS dependent 1 */
    __le32  i_block[EXT2_N_BLOCKS];/* Pointers to blocks */
    __le32  i_generation; /* File version (for NFS) */
    __le32  i_file_acl;   /* File ACL */
    __le32  i_dir_acl;    /* Directory ACL */
    __le32  i_faddr;      /* Fragment address */
    union {
        struct {
            __u8    l_i_frag;   /* Fragment number */
            __u8    l_i_fsize;  /* Fragment size */
            __u16   i_pad1;
        } linux2;
        struct {
            __u8    h_i_frag;   /* Fragment number */
            __u8    h_i_fsize;  /* Fragment size */
            __le16  h_i_mode_high;
            __le16  h_i_uid_high;
            __le16  h_i_gid_high;
            __le32  h_i_author;
        } hurd2;
        struct {
            __u8    m_i_frag;   /* Fragment number */
            __u8    m_i_fsize;  /* Fragment size */
            __u16   m_pad1;
            __u32   m_i_reserved2[2];
        } masix2;
    } osd2;               /* OS dependent 2 */
};

/*
 * Constants relative to the data blocks
 */
#define EXT2_NDIR_BLOCKS    12
#define EXT2_IND_BLOCK      EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK     (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK     (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS       (EXT2_TIND_BLOCK + 1)

备注: EXT2_N_BLOCKS = 15
```

**再次注意**：
- 文件名属性并未纳入到inode数据结构内部。
- inode的大小一般是128字节或者256，我们后面统一128字节 。
- 任何文件的内容大小可以不同，但是属性大小一定是相同的 。

到目前为止，相信大家还有两个问题：
1. 我们已经知道硬盘是典型的 “块” 设备，操作系统读取硬盘数据的时候，读取的基本单位是”块”。“块” 又是硬盘的每个分区下的结构，难道 “块” 是随意的在分区上排布的吗？那要怎么找到“块”呢？
2. 还有就是上面提到的存储文件属性的inode，又是如何放置的呢？

文件系统就是为了组织管理这些的！！

## 3. ext2文件系统 


### 3-1 宏观认识
所有的准备工作都已经做完，是时候认识下文件系统了。

我们想要在硬盘上储文件，必须先把硬盘格式化为某种格式的文件系统，才能存储文件。

**文件系统的目的就是组织和管理硬盘中的文件**。

在Linux系统中，最常见的是ext2系列的文件系统。其早期版本为ext2，后来又发展出ext3和ext4。ext3和ext4虽然对ext2进行了增强，但是其核心设计并没有发生变化，我们仍是以较老的ext2作为演示对象。

ext2文件系统将整个分区划分成若干个同样大小的块组（Block Group）。只要能管理一个分区就能管理所有分区，也就能管理所有磁盘文件。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/adff58bbfde243d1b75ee28f91fef689.png)

上图中启动块（Boot Block/Sector）的大小是确定的，为1KB，由PC标准规定，**用来存储磁盘分区信息和启动信息，任何文件系统都不能修改启动块**。启动块之后才是ext2文件系统的开始。

### 3-2 Block Group
ext2文件系统会根据分区的大小划分为数个Block Group。而每个Block Group都有着相同的结构组成。

### 3-3 块组内部构成
#### **3-3-1 超级块（Super Block）**
**存放文件系统本身的结构信息，描述整个分区的文件系统信息**。

记录的信息主要有：block和inode的总量，未使用的block和inode的数量，一个block和inode的大小，最近一次挂载的时间，最近一次写入数据的时间，最近一次检验磁盘的时间等其他文件系统的相关信息。Super Block的信息被破坏，可以说整个文件系统就被破坏了。

超级块在每个块组的开头都有物理拷贝（第一个块组必须有，后面的块组可以没有）。

为了保证文件系统在磁盘部分扇区出现物理问题的情况下还能正常工作，就必须保证文件系统的super block信息在这种情况下也能正常访问。所以一个文件系统的super block会在多个block group中进行备份，这些super block区域的数据保持一致。

```c
/*
 * Structure of the super block
 */
struct ext2_super_block {
    __le32  s_inodes_count;       /* Inodes count */
    __le32  s_blocks_count;       /* Blocks count */
    __le32  s_r_blocks_count;     /* Reserved blocks count */
    __le32  s_free_blocks_count;  /* Free blocks count */
    __le32  s_first_data_block;   /* First Data Block */
    __le32  s_log_block_size;     /* Block size */
    __le32  s_log_frag_size;      /* Fragment size */
    __le32  s_blocks_per_group;   /* # Blocks per group */
    __le32  s_frags_per_group;    /* # Fragments per group */
    __le32  s_inodes_per_group;   /* # Inodes per group */
    __le32  s_mtime;              /* Mount time */
    __le32  s_wtime;              /* Write time */
    __le16  s_mnt_count;          /* Mount count */
    __le16  s_max_mnt_count;      /* Maximum mount count */
    __le16  s_magic;              /* Magic signature */
    __le16  s_state;              /* File system state */
    __le16  s_errors;             /* Behavior when detecting errors */
    __le16  s_minor_rev_level;    /* minor revision level */
    __le32  s_lastcheck;          /* time of last check */
    __le32  s_checkinterval;      /* max. time between checks */
    __le32  s_rev_level;          /* Revision level */
    __le16  s_def_resuid;         /* Default uid for reserved blocks */
    __le16  s_def_resgid;         /* Default gid for reserved blocks */

    /*
     * These fields are for EXT2_DYNAMIC_REV superblocks only.
     *
     * Note: the difference between the compatible feature set and
     * the incompatible feature set is that if there is a bit set
     * in the incompatible feature set that the kernel doesn't
     * know about, it should refuse to mount the filesystem.
     *
     * e2fsck's requirements are more strict; if it doesn't know
     * about a feature, it must abort and not try to meddle with
     * things it doesn't understand...
     */
    __le32  s_first_ino;          /* First non-reserved inode */
    __le16  s_inode_size;         /* size of inode structure */
    __le16  s_block_group_nr;     /* block group # of this superblock */
    __le32  s_feature_compat;     /* compatible feature set */
    __le32  s_feature_incompat;   /* incompatible feature set */
    __le32  s_feature_ro_compat;  /* readonly-compatible feature set */
    __u8    s_uuid[16];           /* 128-bit uuid for volume */
    char    s_volume_name[16];    /* volume name */
    char    s_last_mounted[64];   /* directory where last mounted */
    __le32  s_algorithm_usage_bitmap; /* For compression */

    /*
     * Performance hints.  Directory preallocation should only
     * happen if the EXT2_COMPAT_PREALLOC flag is on.
     */
    __u8    s_prealloc_blocks;    /* Nr of blocks to try to preallocate*/
    __u8    s_prealloc_dir_blocks;/* Nr to preallocate for dirs */
    __u16   s_padding1;

    /*
     * Journaling support valid if EXT3_FEATURE_COMPAT_HAS_JOURNAL set.
     */
    __u8    s_journal_uuid[16];   /* uuid of journal superblock */
    __u32   s_journal_inum;       /* inode number of journal file */
    __u32   s_journal_dev;        /* device number of journal file */
    __u32   s_last_orphan;        /* start of list of inodes to delete */
    __u32   s_hash_seed[4];       /* HTREE hash seed */
    __u8    s_def_hash_version;   /* Default hash version to use */
    __u8    s_reserved_char_pad;
    __u16   s_reserved_word_pad;
    __le32  s_default_mount_opts;
    __le32  s_first_meta_bg;      /* First metablock block group */
    __u32   s_reserved[190];      /* Padding to the end of the block */
};
```

#### **3-3-2 GDT（Group Descriptor Table）**
块组描述符表，**描述块组属性信息**，整个分区分成多个块组就对应有多少个块组描述符。每个块组描述符存储一个块组的描述信息，如在整个分区中从哪里开始是inode Table，从哪里开始是Data Blocks，空闲的inode和数据块还有多少个等等。块组描述符在每个块组的开头都有一份拷贝。

```c
// 磁盘级blockgroup的数据结构
/* Structure of a blocks group descriptor */
struct ext2_group_desc
{
    __le32  bg_block_bitmap;      /* Blocks bitmap block */
    __le32  bg_inode_bitmap;      /* Inodes bitmap block */
    __le32  bg_inode_table;       /* Inodes table block */
    __le16  bg_free_blocks_count; /* Free blocks count */
    __le16  bg_free_inodes_count; /* Free inodes count */
    __le16  bg_used_dirs_count;   /* Directories count */
    __le16  bg_pad;
    __le32  bg_reserved[3];
};
```

#### **3-3-3 块位图（Block Bitmap）**
Block Bitmap中记录着Data Block中哪个数据块已经被占用，哪个数据块没有被占用。

#### **3-3-4 inode位图（Inode Bitmap）**
每个bit表示一个inode是否空闲可用。

**3-3-5 inode表（inode Table）**
- 存放文件属性，如文件大小，所有者，最近修改时间等。
- 当前分组所有inode属性的集合。
- inode编号以分区为单位，整体划分，不可跨分区。

#### **3-3-6 数据块（Data Block）**
- 存放文件内容，也就是一个个的Block。根据不同的文件类型有以下几种情况：
  - 对于普通文件，文件的数据存储在数据块中。
  - 对于目录，该目录下的所有文件名和目录名存储在所在目录的数据块中，除了文件名外，ls -l命令看到的其它信息保存在该文件的inode中。
- block号按照分区划分，不可跨分区。

### 3-4 inode和datablock映射（弱化）
`ext2_inode`中`i_block[EXT2_N_BLOCKS]`就是用来进行inode和block映射的。

这样文件=内容+属性，就能找到了。 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/f3a36e152eb34c059411e2bfe7ff9598.png)

请解释：知道inode号的情况下，在指定分区，对文件进行增、删、查、改是在做什么？
- 增
内核找空闲inode存文件属性，找空闲数据块存内容，记录块分配情况，在目录添加文件名 - inode映射。
- 删
删目录中文件名 - inode映射，标记inode及对应数据块为空闲。 
- 查
读inode获属性，依inode指针找数据块读内容。 
- 改
改属性直接更新inode；改内容若大小不变/变小，覆盖原数据块；变大则找新块，更新inode映射。 


**结论**
- 分区后的格式化操作，是对分区进行分组，在每个分组中写入SB、GDT、Block Bitmap、Inode Bitmap等管理信息，这些管理信息统称：文件系统。
- 只要知道文件的inode号，就能在指定分区中确定是哪一个分组，进而在哪一个分组确定是哪一个inode。
- 拿到inode文件属性和内容就全都有了。

下面，通过touch一个新文件来看看如何工作。

```bash
[root@localhost linux]# touch abc
[root@localhost linux]# ls -i abc
263466 abc
```

为了说明问题，我们将上图简化：
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/d6ab2bee0ae246afa46b9254cca684bc.png)


创建一个新文件主要有以下4个操作：
1. **存储属性**：内核先找到一个空闲的i节点（这里是263466）。内核把文件信息记录到其中。
2. **存储数据**：该文件需要存储在三个磁盘块，内核找到了三个空闲块：300,500, 800。将内核缓冲区的第一块数据复制到300，下一块复制到500，以此类推。
3. **记录分配情况**：文件内容按顺序300,500,800存放。内核在inode上的磁盘分布区记录了上述块列表。
4. **添加文件名到目录**：新的文件名abc。linux如何在当前的目录中记录这个文件？内核将入口（263466，abc）添加到目录文件。文件名和inode之间的对应关系将文件名和文件的内容及属性连接起来。

### 3-5 目录与文件名
**问题**：
- 我们访问文件，都是用的文件名，没用过inode号啊？
- 目录也是文件吗？如何理解？

**答案**：
- 目录也是文件，但是磁盘上没有目录的概念，只有文件属性+文件内容的概念。
- 目录的属性不用多说，内容保存的是：**文件名和inode号的映射关系**。

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc!= 2) {
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    DIR *dir = opendir(argv[1]);  // 系统调用，自行查阅
    if (!dir) {
        perror("opendir");
        exit(EXIT_FAILURE);
    }

    struct dirent *entry;
    while ((entry = readdir(dir))!= NULL) {  // 系统调用，自行查阅
        // Skip the "." and ".." directory entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        printf("Filename: %s, Inode: %lu\n", entry->d_name, (unsigned long)entry->d_ino);
    }

    closedir(dir);
    return 0;
}
```

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b6e447689fe54964b416173ce0646b60.png)


- 所以，访问文件，必须打开当前目录，根据文件名，获得对应的inode号，然后进行文件访问。
- 所以，访问文件必须要知道当前工作目录，本质是必须能打开当前工作目录文件，查看目录文件的内容。

比如：要访问test.c，就必须打开test（当前工作目录），然后才能获取test.c对应的inode进而对文件进行访问。




### 3-6 路径解析
**问题**：打开当前工作目录文件，查看当前工作目录文件的内容？当前工作目录不也是文件吗？我们访问当前工作目录不也是只知道当前工作目录的文件名吗？要访问它，不也得知道当前工作目录的inode吗？

**答案1**：所以也要打开：当前工作目录的上级目录，额……，上级目录不也是目录吗？不还是上面的问题吗？

**答案2**：所以类似“递归”，需要把路径中所有的目录全部解析，出口是“/”根目录。

**最终答案3**：**而实际上，任何文件，都有路径**，访问目标文件，比如：/home/whb/code/test/test/test.c都要从根目录开始，依次打开每一个目录，根据目录名，依次访问每个目录下指定的目录，直到访问到test.c。这个过程叫做Linux路径解析。

**注意**：
- 所以，我们知道了：访问文件必须要有**目录+文件名=路径**的原因
- 根目录固定文件名，inode号，无需查找，系统开机之后就必须知道

**可是路径谁提供？**
- 你访问文件，都是指令/工具访问，本质是进程访问，进程有CWD！进程提供路径。
- 你open文件，提供了路径

**可是最开始的路径从哪里来？**
- 所以Linux为什么要有根目录，根目录下为什么要有那么多缺省目录？
- 你为什么要有家目录，你自己可以新建目录？
- 上面所有行为：**本质就是在磁盘文件系统中，新建目录文件**。而你新建的任何文件，都在你或者系统指定的目录下新建，这不就是天然就有路径了嘛！
- 系统+用户共同构建Linux路径结构.

### 3-7 路径缓存
**问题1**：Linux磁盘中，存在真正的目录吗？
**答案**：不存在，只有文件。只保存文件属性+文件内容

**问题2**：访问任何文件，都要从/目录开始进行路径解析？
**答案**：原则上是，但是这样太慢，所以**Linux会缓存历史路径结构**

**问题3**：Linux目录的概念，怎么产生的？
**答案**：打开的文件是目录的话，由OS自己在内存中进行路径维护

Linux中，在内核中维护树状路径结构的内核结构体叫做：`struct dentry`

```c
struct dentry {
    atomic_t d_count;
    unsigned int d_flags;     /* protected by d_lock */
    spinlock_t d_lock;        /* per dentry lock */
    struct inode *d_inode;    /* Where the name belongs to - NULL is negative */
    /*
     * The next three fields are touched by __d_lookup. Place them here
     * so they all fit in a cache line.
     */
    struct hlist_node d_hash; /* lookup hash list */
    struct dentry *d_parent;  /* parent directory */
    struct qstr d_name;
    void *d_fsdata;
    struct list_head d_lru;   /* LRU list */
    /*
     * d_child and d_rcu can share memory
     */
    union {
        struct list_head d_child; /* child of parent list */
        struct rcu_head d_rcu;
    } d_u;
    struct list_head d_subdirs; /* our children */
    struct list_head d_alias;  /* inode alias list */
    unsigned long d_time;      /* used by d_revalidate */
    struct dentry_operations *d_op;
    struct super_block *d_sb;  /* The root of the dentry tree */
    void *d_fsdata;            /* fs - specific data */
#ifdef CONFIG_PROFILING
    struct dcookie_struct *d_cookie; /* cookie, if any */
#endif
    int d_mounted;
    unsigned char d_iname[DNAME_INLINE_LEN_MIN]; /* small names */
};
```

**注意**：
- 每个文件其实都要有对应的`dentry`结构，包括普通文件。这样所有被打开的文件，就可以在内存中形成整个树形结构
- 整个树形节点也同时会隶属于LRU(Least Recently Used，最近最少使用)结构中，进行节点淘汰
- 整个树形节点也同时会隶属于Hash，方便快速查找
- 更重要的是，这个树形结构，整体构成了Linux的路径缓存结构，打开访问任何文件，都在先在这棵树下根据路径进行查找，找到就返回属性inode和内容，没找到就从磁盘加载路径，添加dentry结构，缓存新路径 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/82ae4b1a5b284e10b471c64f5ba4ff65.png)

### 3-8 挂载分区

我们已经能够根据inode号在指定分区找文件了，也已经能根据目录文件内容，找指定的inode了，在指定的分区内，我们可以为所欲为了。

可是：
问题：inode不是不能跨分区吗？Linux不是可以有多个分区吗？我怎么知道我在哪⼀个分区？？？
#### 3-8-1 一个实验：
```bash
$ dd if=/dev/zero of=./disk.img bs=1M count=5  #制作一个大的磁盘块，就当做一个分区
$ mkfs.ext4 disk.img    # 格式化写入文件系统
$ mkdir /mnt/mydisk     # 建立空目录
$ df -h                 # 查看可以使用的分区
Filesystem      Size  Used Avail Use% Mounted on
udev            956M     0  956M   0% /dev
tmpfs           198M  724K  197M   1% /run
/dev/vda1        50G   20G   28G  42% /
tmpfs           986M     0  986M   0% /dev/shm
tmpfs           5.0M     0  5.0M   0% /run/lock
tmpfs           986M     0  986M   0% /sys/fs/cgroup
tmpfs           198M     0  198M   0% /run/user/0
tmpfs           198M     0  198M   0% /run/user/1002
$ sudo mount -t ext4./disk.img /mnt/mydisk/  # 将分区挂载到指定的目录
$ df -h
Filesystem      Size  Used Avail Use% Mounted on
udev            956M     0  956M   0% /dev
tmpfs           198M  724K  197M   1% /run
/dev/vda1        50G   20G   28G  42% /
tmpfs           986M     0  986M   0% /dev/shm
tmpfs           5.0M     0  5.0M   0% /run/lock
tmpfs           986M     0  986M   0% /sys/fs/cgroup
tmpfs           198M     0  198M   0% /run/user/0
tmpfs           198M     0  198M   0% /run/user/1002
/dev/loop0       4.9M   24K  4.5M   1% /mnt/mydisk
$ sudo umount /mnt/mydisk  #卸载分区
whb@bite:/mnt$ df -h
Filesystem      Size  Used Avail Use% Mounted on
udev            956M     0  956M   0% /dev
tmpfs           198M  724K  197M   1% /run
/dev/vda1        50G   20G   28G  42% /
tmpfs           986M     0  986M   0% /dev/shm
tmpfs           5.0M     0  5.0M   0% /run/lock
tmpfs           986M     0  986M   0% /sys/fs/cgroup
tmpfs           198M     0  198M   0% /run/user/0
tmpfs           198M     0  198M   0% /run/user/1002
```

**注意**：
`/dev/loop0`在Linux系统中代表第一个循环设备（loop device）。循环设备，也被称为回环设备或者loopback设备，是一种伪设备（pseudo-device），它允许将文件作为块设备（block device）来使用。这种机制使得可以将文件（比如ISO镜像文件）挂载（mount）为文件系统，就像它们是物理硬盘分区或者外部存储设备一样。

```bash
whb@bite:/mnt$ ls /dev/loop* -l
brw-rw---- 1 root disk  7,  0 Oct 17 18:24 /dev/loop0
brw-rw---- 1 root disk  7,  1 Jul 17 10:26 /dev/loop1
brw-rw---- 1 root disk  7,  2 Jul 17 10:26 /dev/loop2
brw-rw---- 1 root disk  7,  3 Jul 17 10:26 /dev/loop3
brw-rw---- 1 root disk  7,  4 Jul 17 10:26 /dev/loop4
brw-rw---- 1 root disk  7,  5 Jul 17 10:26 /dev/loop5
brw-rw---- 1 root disk  7,  6 Jul 17 10:26 /dev/loop6
brw-rw---- 1 root disk  7,  7 Jul 17 10:26 /dev/loop7
crw-rw---- 1 root disk 10, 237 Jul 17 10:26 /dev/loop-control
```

#### 3-8-2 相关概念介绍
在Linux系统中，挂载分区是使操作系统能够访问存储设备（如硬盘分区、U盘、光盘等）上文件系统的过程，以下是相关介绍：
##### 挂载的原理
存储设备在物理上存储数据，但操作系统**需要通过特定机制将设备上的文件系统与自身文件目录结构关联起来，这个关联过程就是挂载** 。挂载后，**设备上的文件系统成为操作系统目录树的一部分**，用户可像访问本地目录一样访问设备中的文件 。
##### 挂载分区的步骤
1. **确定设备标识**：每个存储设备或分区在Linux系统中有特定标识。硬盘分区一般在`/dev`目录下，如`/dev/sda1`（`sda`表示第一个SCSI硬盘，`1`表示第一个分区 ）；U盘插入后可能是`/dev/sdb1` ；光盘可能是`/dev/cdrom` 。可通过`lsblk`命令查看系统中块设备信息及对应分区情况。
2. **创建挂载点**：挂载点是Linux文件系统中用于关联存储设备文件系统的目录 。通常在`/mnt`或`/media`目录下创建，如`mkdir /mnt/mydisk`创建用于挂载磁盘分区的目录 。
3. **执行挂载操作**：使用`mount`命令挂载分区，格式为`mount -t 文件系统类型 设备标识 挂载点` 。例如挂载`/dev/sda1`分区（假设为ext4文件系统 ）到`/mnt/mydisk` ，命令是`mount -t ext4 /dev/sda1 /mnt/mydisk` 。若不指定文件系统类型，`mount`命令会尝试自动检测 。
##### 常见文件系统类型及挂载选项
 - **常见文件系统类型**：
    - **ext4**：Linux主流文件系统，性能好、可靠性高，支持大容量存储和日志功能 。
    - **NTFS**：Windows常用文件系统，Linux通过安装`ntfs - 3g`软件包可读写NTFS分区 。
    - **FAT32**：广泛用于U盘、移动硬盘等，兼容性好，支持多种操作系统，但对单个文件大小有限制 。
 - **挂载选项**：`mount`命令可带多种选项，如`-o ro`设置分区为只读挂载 ；`-o rw`为读写挂载（默认 ）；`-o remount`可重新挂载已挂载分区，用于修改挂载选项等操作 。例如`mount -o remount,ro /dev/sda1`将`/dev/sda1`重新挂载为只读 。 
##### 自动挂载
每次手动挂载不便，可设置自动挂载 。在`/etc/fstab`文件中配置存储设备信息、挂载点、文件系统类型及挂载选项等 。格式为`设备标识 挂载点 文件系统类型 挂载选项 转储频率 文件系统检查顺序` 。例如`/dev/sda1 /mnt/mydisk ext4 defaults 0 0` ，系统启动时会按此配置自动挂载 。 

#### 3-8-3 一个结论
- 分区写入文件系统，无法直接使用，需要和指定的目录关联，进行挂载才能使用。
- 所以，可以根据**访问目标文件的“路径前缀”准确判断在哪一个分区**。 
##  文件系统总结
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/7d7b8464fe0048b5a69d7f41b42c6b04.png)
>在Linux内核中，`fs_struct`结构体用于描述进程与文件系统相关的信息，主要包含以下方面：
>**作用**
>  - 它记录了进程当前工作目录（current working directory）和根目录（root directory）的信息 。通过这些信息，内核能够确定进程在文件系统中的位置，使得进程在执行文件操作（如打开、创建文件等）时，能准确找到相应文件路径 。
>  
>**关键成员**
>  - **root**：指向进程根目录对应的`vfsmount`和`dentry`结构体 。`vfsmount`用于描述文件系统的挂载信息，`dentry`用于表示目录项 。通过这两个结构体，内核可以追踪到进程根目录在文件系统中的具体挂载位置和目录项信息 。
>  - **pwd**：指向进程当前工作目录对应的`vfsmount`和`dentry`结构体 。这使得内核能知晓进程当前所处的工作目录位置，当进程执行相对路径的文件操作时，以此为基础进行路径解析 。
>  - **umask**：记录进程的文件权限掩码 。在进程创建新文件或目录时，该掩码用于确定新创建文件或目录的默认权限 。
>  
>**工作机制**
>  - 当进程创建时，会从父进程继承`fs_struct`相关信息 。之后，进程可通过系统调用（如`chroot`改变根目录 、`chdir`改变当前工作目录 ）来修改自身`fs_struct`中的信息 。例如，使用`chdir`系统调用时，内核会根据传入的路径参数，查找对应的`dentry`和`vfsmount`，并更新`fs_struct`中`pwd`所指向的内容 。在文件路径解析过程中，内核依据`fs_struct`中记录的根目录和当前工作目录信息，结合文件名或相对路径，逐步找到目标文件或目录 。 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/9ef6f891404f4b2f93706752b809bb90.jpeg)
## 4. 软硬连接
### 4-1 硬链接
我们看到，真正找到磁盘上文件的并不是文件名，而是inode。其实在linux中可以让多个文件名对应于同一个inode。

```bash
[root@localhost linux]# touch abc
[root@localhost linux]# ln abc def
[root@localhost linux]# ls -li abc def
263466 abc
263466 def
```
- abc和def的链接状态完全相同，他们被称为指向文件的硬链接。内核记录了这个连接数，inode 263466的硬连接数为2。
- 我们在删除文件时干了两件事情：1.在目录中将对应的记录删除，2.将硬连接数-1，如果为0，则将对应的磁盘释放。

#### 4-2 软链接
硬链接是通过inode引用另外一个文件，软链接是通过名字引用另外一个文件，但实际上，新的文件和被引用的文件的inode不同，应用常见上可以想象成一个快捷方式。在shell中的做法

```bash
[root@localhost linux]# ln -s abc.s abc
```

```bash
[root@localhost linux]# ls -li
263563 -rw-r--r--. 2 root root 0 9月  15 17:45 abc
261678 lrwxrwxrwx. 1 root root 3 9月  15 17:53 abc.s -> abc
263563 -rw-r--r--. 2 root root 0 9月  15 17:45 def
```

下面解释一下文件的三个时间：
- Access最后访问时间
- Modify文件内容最后修改时间
- Change属性最后修改时间

#### 4-3 软硬连接对比
- 软连接是独立文件
- 硬链接只是文件名和目标文件inode的映射关系

#### 4-4 软硬连接的用途：
**硬链接**
- `.`和`..`就是硬链接
- 文件备份

**软连接**
- 类似快捷方式 
