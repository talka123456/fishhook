fishhook 动态 hook 原理 & 源码解析
众所周知 `fishhook`是基于动态库 bind 机制实现的一个 hook 库，这里详解的剖析一下源码和技术细节。
首先 fishhook 定义的数据结构主要是两个 `rebindings_entry` 和 `rebinding`, 前者定义的是一个链接节点，后者是开放给开发者的 api
`rebinding`包含三个变量：
- const char *name：用于在字符串表中根据名称查找符号 
- void *replacement： 记录新的重定向的函数 IMP
- void **replaced：保存被 hook 函数的 IMP

prepend_rebindings函数主要是预处理数据结构，采用头插法生成链表保存 hook 数据。


## 数据结构
`
#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
`

这里举例 64 位数据结构， 
- mach_header_64： MachO 文件的 header,对应 MachOView 中看到的 Mach64 Header 段
- segment_command_64： MachO Segment_64类型的 LC, 对应 MachOView 中 LC_SEGMENT_64类型的 LC, 包括__TEXT、__DATA_CONST、__DATA、__LINKEDIT
- section_64: MachO 中 section 数据的结构，对应 MachOView 中的 Section64 段
- nlist_64： 和符号表相关的 section 数据结构。例如 symbol table

> 间接符号表和字符串表在使用地址信息时，使用数组指针表示。例如 char *strtab 和 uint32_t *indirect
> _symtab
## 
细节一 _dyld_register_func_for_add_image
_dyld_register_func_for_add_image要求的回调函数参数格式是固定的，包含 header 和 slide.所以 _rebind_symbols_for_image 函数的作用就是适配回调参数格式，并封装参数调用入口函数。

细节二 Linkedit Base Addr
#define    SEG_LINKEDIT    "__LINKEDIT" /* 包含需要被动态链接器使用的符号和其他表，包括符号表、字符串表等 */
**为什么 Linkedit Segment 首地址信息十分重要？ 因为在 Load Command 中，LC_SYMTAB 和 LC_DYSYMTAB 的中所记录的 Offset 都是基于 __LINKEDIT 段的**

在介绍这段代码意义之前，先来分析一下这几个字段值的函数
- vmaddr：未使用 aslr 时，加载到虚拟内存的地址，
- fileoff：在当前文件中偏移地址（如果存在多个架构文件， 表示的是当前架构中的偏移）

它描述了文件映射的两大问题：从哪里来（fileoff、filesize）、到哪里去（vmaddr、vmsize。【Mach-O 简单分析 - 张不坏的博客】

fishhook 绝大多数重要的地址计算都要使用到或是间接使用到 Linkedit Base Addr； **Linkedit Segment 在文件中的首地址。**
在segment_64类型的LC中，都存在 vmaddr和  file offset 这样的字段， 表示映射到虚拟内存后的地址以及在文件中的偏移,

细节三： GOT & GOT.plt 中的指针如何关联符号表的。
对于 GOT 表和 GOT.plt 中的内容， LC_SEGMENT_64中 Section64 Header(__got)中的保留字段`reverved1`代表的是在间接符号符号表中的索引，后续__got 中每一个指针按顺序对应间接符号表的符号。
根据间接符号表 base addr + index,可以获取 GOT 表中的指针代表的符号含义。

**__got 中是读取的 Section Header 中的reserved1做对应，然后按顺序对应间接符号表。**

细节四： 符号匹配是，将符号表中的符号首字符去除，因为是‘_’，Hook填写的符号名是没有 name mangling
细节五： 判断被 hook 的函数新绑定的函数不为空（未保留原函数实现的行为不支持），&& 重定向的 IMP 不等于当前的函数 IMP(即 hook 到自身实现的行为不支持)

细节六： &func 和*func, 前者是创建一个指向 func 函数的指针 p， 后者是取指针的值（即函数的IMP）赋值替换。这样在调用 func 时，指向的是新的地址。
细节七： 在重新赋值之前，还检查了内存的权限，将内存权限改为读写和可拷贝。
## 逻辑
1.针对__got 中查找到的每一个符号都比对链表中的所有值，这里是 while 嵌套 for实现的。所以为什么不使用 map 结构来快速查询，
如果多次添加 hook 同一函数，处理顺序问题由于是头插法，所以后续 hook 的同名函数，会生效。
但是同一个 rebindings 中， 是顺序迭代的，意味着放在前面的生效。

