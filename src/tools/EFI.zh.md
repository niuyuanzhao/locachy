# UEFI 启动项

[English](EFI.md)

已安装系统使用名为 `LOCachy` 的正式 UEFI 启动项，指向
`\EFI\locachy\locachy.efi`。`EFI/BOOT/BOOTX64.EFI` 保留为可移动介质标准后备，
不再是唯一正常启动入口。

安装器写好两份映像后，运行已安装源码的 `locachy-efi --esp TARGET --first`，
登记所选 ESP，把 LOCachy 放到 BootOrder 首位，保留所有其他条目及其相对顺序。
固件变量不可用或不可写时明确警告；目标映像仍安装完成，fallback 保留。

这些改动仍在本地，尚未发布。既有系统首次进行源码优先更新时，先装包管理工具，
之后才完成 LOCachy 编译；前面的 pacman 钩子仍可能运行旧引导工具。更新完成
`make.sh` 后才装好新的 EFI 工具并启用登记路径，不另加包管理 bootstrap 安装步骤。

普通 `locachy-boot` 映像更新也检查启动项：复用完全匹配的条目，必要时启用，
若不在 BootOrder 中就追加，不反复抢占用户选择的首位。不存在匹配项时才新建。
旧的异盘条目即使也叫 LOCachy，也只报告、不删除。该工具不更改 BootNext、
其他操作系统的条目或安全启动密钥。安装登记发生在最终 ESP 映像及所请求的
安全启动登记之后、记录安装完成之前。Boot####/BootOrder 可能影响固件度量，例如
PCR 1；LOCachy 默认绑定不使用 PCR 1（自有密钥为 PCR 7 加签名 PCR 11，
仅 TPM 为 0+7，TPM 1.2 为 0）。自定义 PCR 策略可能需要重新绑定。
下面的 OVMF 测试未组合 TPM/安全启动下的登记验证。

已安装系统仅修复启动项，在 root 下运行：

```sh
locachy-boot --register-only
```

不会重建或替换映像。显式修复失败时退出非零，与普通映像更新的警告不同。
`locachy-efi --esp /efi --first` 可明确将它再设为首位；普通更新从不要求此优先级。

## 身份与失败规则

工具使用标准 `findmnt`、`lsblk --properties-by blkid` 及 `efibootmgr`，不直接写
EFI 变量。目标必须是 GPT EFI System Partition 上显式挂载的 FAT 文件系统，
挂载 major/minor 与块节点相符，父设备必须是磁盘，且非符号链接的引导映像存在。
分区号和 PARTUUID 来自块元数据，不截字符串猜设备名，因此 SATA、NVMe、MMC
走同一条路径。

只复用标签、GPT UUID、分区号、映像路径均匹配的条目。`--create-only` 避免
新建时隐式改变 BootOrder，随后明确写入所需顺序并回读验证。部分失败可重试：
若已建项但写顺序失败，下次复用已有项，不再重复创建。Legacy 启动、efivarfs
缺失或只读、不支持的目标、命令错误及固件未保存状态都明确失败。普通映像更新
中，这些错误不会丢弃已经成功更新的 UKI 和 fallback。普通更新在写 ESP 文件前还
确认 `/efi` 是实际 FAT 挂载点；根文件系统上的遗留目录不算挂载成功。

## 测试

`python3 -B tools/test-efi.py SCRATCH_PARENT` 的 15 项测试通过，用替身覆盖目标身份、SATA/NVMe/MMC、
File(PATH) 和直接文件路径两种输出格式、幂等、启用与补顺序、错误目标/标签、
失败及调用顺序。

Linux 开发机上的真实隔离测试：

```sh
bash tools/test-efi-vm.sh RUNTIME_ROOT PACKAGE_CACHE NEW_OUTPUT
```

启动无盘的 2-vCPU/2-GiB OVMF 客机，使用独立 NVRAM 和一次性 128-MiB GPT ESP。
运行时、源码及缓存只读，缓存中的 ukify/pefile 只在客机 overlay 解包。不暴露
宿主固件、显示、输入或网络。实际写入、回读 Boot#### 与 BootOrder，验证安装首位、
重复无重复项、禁用/缺顺序修复、删除后 `--register-only` 补建，以及真实重建引导
映像后保留人为调整的顺序。

通过的记录位于 `~/x230-dev/efi-agent/vm-guard/`：`EFI_VM_STATUS=0`，五项功能 PASS
及 `PASS EFI VM complete`，日志及前后 NVRAM 文本保留。测试对象是真实 efibootmgr
与 OVMF，不是实机固件，也没有再次从新登记的映像重启。

参考：[efibootmgr(8)](https://man.archlinux.org/man/efibootmgr.8.en)。
