//
//  ViewController.m
//  HWFishhookDemo
//
//  Created by 我家有个小胖妞 on 2022/3/18.
//

#import "ViewController.h"
#import "fishhook.h"

@interface ViewController ()

@end

// 定义一个函数，将被 hook 的函数地址绑定到该函数上，方便调用
static void (*ori_nslog)(NSString * format, ...);

void new_nslog(NSString * format, ...) {
    //自定义的替换函数
    format = [format stringByAppendingFormat:@" Gua "];
    // 调用原函数地址。
    ori_nslog(format);
}

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor redColor];
    NSLog(@"hello world");
    
    // 构建一个重新绑定结构体
    struct rebinding nslog;
    // 名称很重要，因为是要根据字符串查找符号
    nslog.name = "NSLog";
    // hook 的新函数地址， 函数名代表函数地址。
    nslog.replacement = new_nslog;
    // 将原来的 IMP 邦定到ori_nslog的目的。这里用&的原因是，fishhook 内在赋值时用了*，是一一对应的。
    // &传入的是一个指向函数的地址，fishhook 通过*改变了该值，将新的值替换了ori_nslog IMP
    nslog.replaced = (void *)&ori_nslog;
    // 重新绑定符号
    rebind_symbols((struct rebinding[1]){nslog}, 1);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    NSLog(@"AAAA");
}

@end
