ex2的硬盘，布局如下图：

| BOOT BLOCK |      BLOCK GROUP0        | BLOCK GROUP 1 | ... | BLOCK GROUP n |
          |                                      |
	   |                                                          |
   | Super BLOCK |   GDT   | BLOCK BITMAP | INODE BITMAP | INODE TABLE | DATA BLOCKs | 
        1个块       N个快       1个块          1个快          N个块         N个块
                            1*blocksize*8   1*blocksize*8  (inode_per_group*inode_size+block_size-1)/block_size
							
硬盘的第一个扇区是引导区，占据1K字节，引导区是文件系统不能使用的，用来存储分区信息。
ext2是通过快组的方式来组织硬盘的。每个硬盘分区都由若干个大小相同的快组组成。

超级块(super block): 每个块组的起始位置都是有个超级块，这些超级块的内容都是相同的。
  超级块包括文件系统的信息，比如每个块组的块数目、每个块组的inode数目等。
  使用struct ext2_super_block。
  
块组描述符(GDT): 块组描述符表由很多的块组描述符组成。ext2的每个块组描述为32字节。
  整个文件系统分区多少个块组，就有多少个组描述符。块组描述符描述了一个块组的信息，
  比如一个块组中inode位图额起始位置、inode表的其实位置。使用structext2_group_desc
  
块位图(Block bitmap): 每个比特代表块组中那些块可用，那些块已经被占用。块位图本身要
  占有一个块。如果块大小设置为1K字节，则一个块组的大小为1K x 1K x 8bit = 8M字节。
  
inode位图(inode Bitmap): inode位图也占用一个块。它的每一个代表一个inode是否空闲。

indoe表(inode table): 每个文件都由一个inode，inode 保存了文件的描述信息、文件的类型、
  文件大小、文件的创建访问时间等。一个indoe占128字节。如果文件系统块大小为1K字节，一个
  块大小为1K字节一个块可容纳8个inode。inode表可以占用多个块。

数据块(Data Block): 保存文件内容。常规文件的数据保存在数据块中，如果目录文件，那么该目
  录下所有的文件名和下级目录名都保存在数据块中。 使用struct ext2_inode