// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "fishhook.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

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
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

// 定义一个链表，保存符号数组， 个数，以及下一个节点指针。
struct rebindings_entry {
  struct rebinding *rebindings;
  size_t rebindings_nel;
  struct rebindings_entry *next;
};

// 链表头结点
static struct rebindings_entry *_rebindings_head;

/// 预处理数据结构，主要是生成链表节点，然后采用头插的方式生成链表
/// @param rebindings_head 链表头结点
/// @param rebindings 被 hook 的结构体数组
/// @param nel 个数
/// return 正常返回 0 ， 错误返回-1
static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                              struct rebinding rebindings[],
                              size_t nel) {
    // 分配空间，存储需要被绑定的结构体信息
  struct rebindings_entry *new_entry = (struct rebindings_entry *) malloc(sizeof(struct rebindings_entry));
  if (!new_entry) {
    return -1;
  }
    // 分配 rebinding * size 大小的数组内存空间，数组首地址赋值给 rebindings。
  new_entry->rebindings = (struct rebinding *) malloc(sizeof(struct rebinding) * nel);
  if (!new_entry->rebindings) {
    free(new_entry);
    return -1;
  }
  
  // 通过 memcpy拷贝数据到新开辟的空间。
  memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
    // 个数
  new_entry->rebindings_nel = nel;
    // 头插法
  new_entry->next = *rebindings_head;
  *rebindings_head = new_entry;
  return 0;
}

#if 0
static int get_protection(void *addr, vm_prot_t *prot, vm_prot_t *max_prot) {
  mach_port_t task = mach_task_self();
  vm_size_t size = 0;
  vm_address_t address = (vm_address_t)addr;
  memory_object_name_t object;
#ifdef __LP64__
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  vm_region_basic_info_data_64_t info;
  kern_return_t info_ret = vm_region_64(
      task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
#else
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT;
  vm_region_basic_info_data_t info;
  kern_return_t info_ret = vm_region(task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &count, &object);
#endif
  if (info_ret == KERN_SUCCESS) {
    if (prot != NULL)
      *prot = info.protection;

    if (max_prot != NULL)
      *max_prot = info.max_protection;

    return 0;
  }

  return -1;
}
#endif

/// perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
/// @param rebindings 符号绑定信息
/// @param section __got section header
/// @param slide aslr
/// @param symtab 符号表
/// @param strtab 字符串表
/// @param indirect_symtab 间接符号表
static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab,
                                           char *strtab,
                                           uint32_t *indirect_symtab) {
    // __got Section Header 中的reserved1代表的是在间接符号表中的索引，然后后续__got和索引处的符号一一对应。
    // 用 uint_32_t 表示数组元素类型，这样每+1 个索引， 会自动按类型大小指向下一个位置。
  uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
    // 已知其 value 是一个指针类型，整段区域用二阶指针来获取（指针地址中存储指针，指向指针的指针）
  void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
    // 遍历所有的值
  for (uint i = 0; i < section->size / sizeof(void *); i++) {
      // 间接符号表中的值， 记录的是在符号表中的索引
    uint32_t symtab_index = indirect_symbol_indices[i];
    if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
        symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
      continue;
    }
      // 符号表中是nlist_64类型的数组，n_un是一个 union,通过n_un.n_strx 获取在字符串表中的索引。
    uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
      // 字符串表中，每一个符号名都是以字符串存储的，'\0'，所以使用 char *可以直接读取到结束位置，而不需要知道符号名的长度。
    char *symbol_name = strtab + strtab_offset;
      // 目的是去除‘_’ c 语言的 name mangling 影响。实际名称是从 1 开始的。
    bool symbol_name_longer_than_1 = symbol_name[0] && symbol_name[1];
    
    struct rebindings_entry *cur = rebindings;
    // while 这一层循环是遍历每一个节点
    while (cur) {
        // for 这一层循环是遍历每一个节点中存储的多个符号。rebindings结构是允许一次 hook多个符号的。
      for (uint j = 0; j < cur->rebindings_nel; j++) {
        if (symbol_name_longer_than_1 && strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
          kern_return_t err;
        
            // 判断被 hook 的函数新绑定的函数不为空（未保留原函数实现的行为不支持），&& 重定向的 IMP 不等于当前的函数 IMP(即 hook 到自身实现的行为不支持)
          if (cur->rebindings[j].replaced != NULL && indirect_symbol_bindings[i] != cur->rebindings[j].replacement)
              // 保留原来的值，将原函数地址 IMP 赋值给指针：cur->rebindings[j].replaced， 用*表示修改原指针地址内存储的值，
            *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];

          /**
           * 1. Moved the vm protection modifying codes to here to reduce the
           *    changing scope.
           * 2. Adding VM_PROT_WRITE mode unconditionally because vm_region
           *    API on some iOS/Mac reports mismatch vm protection attributes.
           * -- Lianfu Hao Jun 16th, 2021
           **/
           // 这里是操作内存权限相关的内容。表示将__GOT中的这块内存空间设置为，读、写、拷贝权限的。
          err = vm_protect (mach_task_self (), (uintptr_t)indirect_symbol_bindings, section->size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
          if (err == KERN_SUCCESS) {
            /**
             * Once we failed to change the vm protection, we
             * MUST NOT continue the following write actions!
             * iOS 15 has corrected the const segments prot.
             * -- Lionfore Hao Jun 11th, 2021
             **/
            // 将replacement 新函数地址赋值给原来的指针。
            indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
          }
            
          goto symbol_loop;
        }
      }
        
    // 处理链表下一个节点
      cur = cur->next;
    }
      
  symbol_loop:;
  }
}

/// 处理入口
/// @param rebindings 绑定信息
/// @param header MachO header
/// @param slide aslr
static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide) {
// 从 Macho header 中读取信息。
  Dl_info info;
  if (dladdr(header, &info) == 0) {
    return;
  }

  // 记录当前指向的LC
  segment_command_t *cur_seg_cmd;
    // 链接相关的段
  segment_command_t *linkedit_segment = NULL;
    // 符号表 tab包含符号和字符串表的偏移和大小
  struct symtab_command* symtab_cmd = NULL;
    // 间接符号表
  struct dysymtab_command* dysymtab_cmd = NULL;

    // 跳过 header部分，直接处理 LC
  uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
      
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
        linkedit_segment = cur_seg_cmd;
      }
    } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
      symtab_cmd = (struct symtab_command*)cur_seg_cmd;
    } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
      dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
    }
  }

  if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
      !dysymtab_cmd->nindirectsyms) {
    return;
  }

  // Find base symbol/string table addresses
    //vmaddr表示实际映射的虚拟内存地址，fileoff 表示在当前文件 MachO 中的偏移， 二者差值等于基地址加载后的虚拟内存地址（不包含 aslr 的）
  uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    // 符号表列表， nlist_t代表一个符号表的模型实体类型。
  nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
    // 字符串表
  char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);

    // 间接符号表
  // Get indirect symbol table (array of uint32_t indices into symbol table)
  uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    // 本次遍历的目的是获取重绑定的两个数据表，分别存储变量和函数的符号符号信息，
  cur = (uintptr_t)header + sizeof(mach_header_t);
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
      // 查询 Segment Name 过滤出 __DATA 或者 __DATA_CONST
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
          strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0) {
        continue;
      }
        // 针对 got 和 lazy symbol ptr 做绑定处理
      for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
        section_t *sect =
          (section_t *)(cur + sizeof(segment_command_t)) + j;
        if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
        if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
      }
    }
  }
}

/// 最终收口到该函数处理绑定逻辑。
/// @param header image macho 的 header
/// @param slide aslr 偏移
static void _rebind_symbols_for_image(const struct mach_header *header,
                                      intptr_t slide) {
    rebind_symbols_for_image(_rebindings_head, header, slide);
}

/// 针对特定 image 的重绑定函数，绑定后释放开辟的堆空间，其他image 就无法绑定这次 hook 的函数。
/// @param header image 的 macho header
/// @param slide aslr 偏移
/// @param rebindings hook 符号数组
/// @param rebindings_nel 个数
int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel) {
    
    struct rebindings_entry *rebindings_head = NULL;
    int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel);
    rebind_symbols_for_image(rebindings_head, (const struct mach_header *) header, slide);
    
    // 释放开辟的空间， 目的是清理数据，这样后续如果再有重绑定，不会将本次的内容绑定到其他 image.
    if (rebindings_head) {
      free(rebindings_head->rebindings);
    }
    free(rebindings_head);
    return retval;
}

/// 绑定入口，针对所有 image list 重绑定
///
int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
    // 预处理链表结构
  int retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);
  if (retval < 0) {
    return retval;
  }
  // If this was the first call, register callback for image additions (which is also invoked for
  // existing images, otherwise, just run on existing images
    // 首次调用绑定时，注册 image 加载的回调。
  if (!_rebindings_head->next) {
      // 加载过image立即调用， 未加载的 image加载时调用, 注意该回调要求的参数格式固定，即const struct mach_header *header 和 intptr_t slide
    _dyld_register_func_for_add_image(_rebind_symbols_for_image);
  } else {
      // 二次调用绑定符号时， 不再注册，直接针对所有已经加载的 image 重新绑定符号即可。
      // 未加载的 image 加载时也会重新绑定新的符号，因为 rebingdings_head 数据结构已经改变。
    uint32_t c = _dyld_image_count();
    for (uint32_t i = 0; i < c; i++) {
      _rebind_symbols_for_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }
  }
  return retval;
}
