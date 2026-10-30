# 磁盘加密、TPM 与安全启动

[English](SECURITY.md)

本文说明加密磁盘、TPM 如何打开它、使用本机自有密钥的安全启动、休眠，以及 fwupd 的主机安全报告。锁屏与认证见旧树的 SECURITY.md（`distro/src/tools/SECURITY.md`），尚未对照本树复核。

## 现有内容

**磁盘。** 安装程序建立 LUKS2 根分区（`locachy-crypt`）和 LUKS2 交换分区（`locachy-swap-crypt`），两者用同一个长口令打开。口令从不删除；TPM 打不开磁盘时就会询问它。

任何地方都没有 PIN。用户已决定：口令足够长，作为唯一需要输入的秘密即可。

**TPM 1.2**（ThinkPad X230）：`locachy-tpm enroll` 用 `TPM_Seal` 把一把随机密钥封装到 PCR 0，封装块存入 `locachy-tpm12` LUKS2 令牌。此部分未改动。

**TPM 2.0，未使用本机自有安全启动密钥**：同样未改动。`systemd-cryptenroll --tpm2-pcrs=0+7` 把密钥封装到固件（PCR 0）以及安全启动的状态和密钥（PCR 7）。`locachy-tpm` 另加 `--tpm2-public-key=`；否则只要 `/etc/systemd/tpm2-pcr-public-key.pem` 存在，`systemd-cryptenroll` 就会自行加上带签名的 PCR 11 策略。

**TPM 2.0，使用本机自有安全启动密钥**：按 Arch Wiki、systemd 和 sbctl 的做法。

- `locachy-tpm secure-boot` 用标准工具生成密钥：`sbctl create-keys` 生成 PK、KEK、db，`ukify genkey` 在 `/etc/systemd/` 生成 PCR 签名密钥对。
- `locachy-boot` 用 `ukify --signtool=systemd-sbsign --secureboot-private-key=… --secureboot-certificate=… --no-sign-kernel` 构建 UKI，并加上 `--pcr-private-key=… --pcr-public-key=… --phases=enter-initrd`。这样映像带有它度量到 `enter-initrd` 为止的 PCR 11 值，并用 PCR 密钥签名（`.pcrsig`、`.pcrpkey`）。initramfs 中有 `/etc/initrd-release`，systemd 的 TPM 令牌靠它找到 stub 放的 `/.extra/tpm2-pcr-signature.json`。
- `locachy-tpm secure-boot --enroll [--microsoft]` 运行 `sbctl enroll-keys`，其中包括 sbctl 自己的 option ROM 检查（它读取 securityfs 中的 TPM 事件日志）。只在设置模式下可用。只有在固件启动的映像（`EFI/locachy/locachy.efi` 和 `EFI/BOOT/BOOTX64.EFI`）与刚签好的那份逐字节相同之后才会登记；否则一开安全启动机器就起不来。
- 磁盘绑定为 `systemd-cryptenroll --tpm2-pcrs=7 --tpm2-public-key=… --tpm2-public-key-pcrs=11 --wipe-slot=tpm2`。新槽位写入之后才删除旧的 TPM 2.0 槽位。
- 1 号进程在打开任何磁盘之前度量 `enter-initrd`，在离开 initramfs 或恢复休眠的系统之前度量 `leave-initrd`。它像 `systemd-pcrphase-initrd.service` 一样使用 `systemd-pcrextend`，并为此挂载 efivarfs。因此密钥只在已签名映像的 initramfs 中打开，在运行中的系统里永远打不开。
- 登记密钥会改变 PCR 7。原先已绑定的磁盘各得到一个专用密钥槽：一把 256 位随机密钥，存于加密根分区上的 `/var/lib/locachy/tpm-pending`。如果 TPM 在运行中的系统里仍能打开磁盘（PCR 0+7 绑定可以），就用现有的 TPM 密钥加这个槽位，否则用口令。下次启动时旧绑定失效，口令询问一次。随后 1 号进程运行 `locachy-tpm enroll --pending`，把磁盘绑定到新的 PCR 7 和签名策略，并删除该槽位及其密钥。安装程序用同一机制（`prepare`），因为 live 系统的 PCR 7 与已安装系统的不同。有绑定在等待时，设置显示"下次启动时绑定……"。

等待中的绑定可在失败后继续：先持久保存随机密钥及 UUID 关联，再添加临时 LUKS 槽位。重试先检查该密钥是否已能打开槽位，不重复添加。缺盘、绑定失败或删槽失败时保留该盘的请求和密钥；成功的盘可单独完成。登记成功后记录状态，清理重试不再需要重新登记。只有确认槽位已删除或不存在后才清除密钥。损坏的请求明确报错，保持原样。这些文件仅 root 可读写，使用同目录原子替换及文件、目录 fsync。

**休眠。** 1 号进程先用交换分区的 TPM 令牌打开它，无需输入任何东西。恢复映像前，根分区也必须已经真正解锁：成功使用其 TPM 令牌，或输入长口令。LUKS header 的令牌元数据不是认证，不能单独授权恢复。根分区和 swap 都在 `leave-initrd` 前解锁；TPM 均成功时仍无需输入，并保留会话锁。失败或伪造的根令牌改问口令，口令尝试失败后绝不从休眠映像释放根密钥。每次启动的 `swapon` 和每次关机都会丢弃（discard）整个交换分区（`SWAP_FLAG_DISCARD`、`DISCARD_ONCE`；`cryptsetup --allow-discards`），恢复过的映像不会留在交换分区里。

**fwupd 的主机安全 ID。** 即 `fwupdmgr security` 的输出。安装程序在加密步骤显示 HSI 及未通过的检查项，这需要 live 介质带有 fwupd（`installer/packages.x86_64`）。设置在"安全"的"主机安全"下显示 HSI；在那里按 Enter，会在终端运行 fwupd 的完整报告，附带 fwupd 提供的修复。

## 第 1 轮审计（2026-10-29）

"自有密钥"指使用本机安全启动密钥的 TPM 2.0 绑定。"PCR 0+7"指不使用这些密钥的 TPM 2.0 绑定，与以前相同。"TPM 1.2"指 X230 的绑定。

| 攻击 | 自有密钥 | PCR 0+7 | TPM 1.2 |
|---|---|---|---|
| 把磁盘从机器中取出 | 挡住：封装的密钥只在本 TPM 中打开。 | 挡住。 | 挡住。 |
| 在本机启动 live USB 或其他系统 | 挡住。安全启动开启时，只有本机 db 密钥签名的映像能启动；使用 `--microsoft` 时微软签名的也能。微软签名的系统（shim、Windows 启动管理器）会在 PCR 7 记下微软 CA 为授权者，shim 还会加入其 MOK 变量，所以 PCR 7 不同。 | **挡不住。** 任何在安全启动关闭时启动的系统都读到相同的 PCR 0 和 7，能解封密钥（`systemd-cryptenroll --unlock-tpm2-device`、tpm2-tools）。 | **挡不住**，PCR 0 同理。 |
| 在固件中关闭安全启动、清除或替换密钥 | 挡住：PCR 7 度量 SecureBoot、PK、KEK、db、dbx。改问口令。 | 绑定本来就是那个状态（见上）。 | 不度量。 |
| 篡改 ESP 上的启动映像（evil maid） | 挡住：未签名或用其他密钥签名的映像无法启动；用本机 db 密钥签名但不是本机 PCR 密钥签名的映像，没有匹配的 PCR 11 签名。 | **挡不住**：映像未签名，被替换的映像自己就能解封密钥。 | **挡不住。** |
| 由加载器传入命令行或额外 initrd | 挡住：安全启动开启时，systemd-stub 忽略对命令行的覆盖（UKI 自带 `.cmdline`），也没有已签名的加载器能传入 initrd。 | **挡不住**：安全启动关闭时，`initrd=` 和 `rdinit=` 能到达内核。 | **挡不住。** |
| 更旧的已签名映像（降级） | **挡不住。** 凡是用本 PCR 密钥和 db 密钥签过的映像都能打开磁盘，ESP 上的 `locachy-prev.efi` 就是其中之一；签名策略没有吊销机制。旧映像若在 `leave-initrd` 之前有漏洞，这一点就有影响。补救（未自动化）：换一把新的 PCR 密钥（`ukify genkey`，再 `locachy-tpm enroll`），使所有旧映像失效。 | 不适用 | 不适用 |
| 运行中系统的 root | `leave-initrd` 之后无法解封。它能用加密根分区上的 PCR 私钥和 db 私钥（`/etc/systemd`、`/var/lib/sbctl`）签名映像，所以被攻陷的 root 能留下一个已签名的后门映像。 | 随时可以解封；它本来就有磁盘。 | 能解封。 |
| 交换分区被盗（磁盘取出） | 挡住，与根分区相同。 | 挡住。 | 挡住。 |
| 休眠中的机器被拿走 | 交换分区无需输入即打开，会话回到锁屏，由锁屏守护，与仅用 TPM 冷启动后相同。先度量 `leave-initrd`，所以恢复的系统无法解封。内核以 `intel_iommu=on` 运行，防 DMA。 | 同自有密钥，但没有 `leave-initrd` 的效果。 | 同 PCR 0+7。 |
| 交换分区绑定 TPM，根分区只用口令 | 本轮已修复。此前 1 号进程会在不问根分区口令的情况下恢复由 TPM 打开的交换分区，映像就交出了根分区密钥。现在任何加密恢复前都要求根分区真实解锁成功，仅向根 header 添加未经认证的 TPM 令牌 JSON 不能绕过口令。 | 同上。 | 同上。 |
| 陈旧的休眠映像或换出的页面 | 在支持 discard 的磁盘上，每次启动和关机都会丢弃。在不支持 discard 的磁盘（机械硬盘）上**不会清除**，它们仍以交换分区的密钥加密保存。SSD 在 TRIM 之后可能仍保留物理副本。经 dm-crypt 透传 discard 会暴露哪些块在用（cryptsetup FAQ 所说的取舍），仅限交换分区。 | 同上。 | 同上。 |
| 嗅探 TPM 总线（独立 TPM） | 被动嗅探：挡住。systemd 用登记时记下的 SRK 为解封会话加盐，秘密加密后才过总线。**挡不住**：主动中间人（复位 TPM 后重放 PCR 值、伪造的 TPM）。 | 同上。 | **挡不住**：TPM 1.2 经 LPC 以明文返回密钥。 |
| 固件被改写（X230 没有 Boot Guard） | **挡不住**：固件是信任根，可以伪造度量。 | **挡不住。** | **挡不住。** |
| 登记了微软密钥（`--microsoft`） | 磁盘仍对微软签名的系统关闭（PCR 7，见上）。sbctl 不登记 dbx，所以已吊销的微软二进制仍**能启动**。这只破坏"只有 LOCachy 能启动"这一说法，不影响磁盘。 | 不适用 | 不适用 |
| 恢复途径 | 口令槽位始终保留。TPM 拒绝时就会询问口令，并以红字说明发生了什么。等待中的绑定所用槽位用的是存放在加密根分区内的 256 位随机密钥，不会从外部增加入口；绑定完成、`enroll` 或 `clear` 时即删除。 | 同上。 | 同上。 |
| 开机不锁屏又用 TPM 解锁 | 安装程序和设置都拒绝这种组合。 | 同上。 | 同上。 |

本轮另修复：

- 存在 PCR 密钥时、在没有自有密钥的情况下做的 TPM 2.0 绑定，仍会带上签名的 PCR 11 策略，因为 systemd-cryptenroll 会自行加载该密钥。现在加 `--tpm2-public-key=`。
- 若 ESP 上没有已签名映像就登记密钥，机器会无法启动。现在会拒绝。
- 重新绑定原先先删旧 TPM 令牌；新登记一旦失败，TPM 解锁就没了。现在新令牌写入后才删旧的。
- 休眠映像没能恢复时，原先只提供口令。现在先试令牌，PCR 0+7 绑定仍能打开。

未解决：

- 降级到更旧的已签名映像（见上）。
- 在安全启动下内核 lockdown 会阻止休眠的内核上休眠的问题。Arch 和 CachyOS 内核不会自行进入 lockdown；本内核的实际表现见虚拟机测试。
- 不支持 discard 的磁盘会保留陈旧的交换分区内容（已加密）。

## 第 2 轮审计：等待绑定的故障恢复

复查发现，首版等待绑定在磁盘暂时缺失或删槽失败时会丢弃恢复状态；先加槽后保存请求也会留下尚未保存 UUID 关联的中断窗口。这是登记可靠性缺陷，不是删除长恢复口令或放宽度量策略的理由。

现在先保存密钥和关联，再更改 header；失败设备保留请求，登记完成与清理完成分别记录，删槽可安全重试。下面的 helper 和真实 TPM 2.0 VM 测试覆盖报告失败的窗口，不证明任意存储损坏下的行为，也不替代硬件登记测试。

## 测试

- `tools/test-tpm.py`：用替身代替固件变量、LUKS 工具和 TPM，检查 locachy-tpm 的规则：
  - 自有密钥即安全启动开启且本 db 证书在 db 中；
  - 自有密钥绑定（PCR 7、签名的 PCR 11 策略、事后删除旧槽位、无 PIN）；
  - 不带 PCR 密钥的 PCR 0+7 绑定，以及 `--pcrs`；
  - 等待中的绑定：密钥槽、权限、无安全启动时等待、用密钥文件绑定、删除；
  - `secure-boot --enroll`：生成密钥、映像先上 ESP、先用 TPM 自有密钥再用口令、sbctl 拒绝时不留等待中的绑定；
  - `status`；
  - 缺盘、删槽失败、prepare/retire 中断、原子保存失败、header I/O 错误及损坏请求的恢复；helper 测试共 16 项。
- 真实 TPM 2.0/LUKS 故障重试：`~/x230-dev/tpm-agent/runs/pending2/serial.log` 记录四项 PASS：header 已写后报告失败、重试复用不增重复槽、隐藏 swap UUID 加 root 删槽失败、恢复后成功重试且保留两个恢复口令。`runs/pending-boot/serial.log` 随后记录重试后的 TPM 自动解锁。真实工具在一次性 VM 内执行，包装器注入报错和缺盘条件。没有模拟真实断电或文件系统故障；原子写入中断窗口用替身测试。
- `tools/test-init-unlock.py` 编译实际 init 解锁/恢复函数，以块设备、TPM 和界面替身验证七项：缺少/伪造根令牌、令牌真正解锁、口令后备、耗尽尝试、无法确认的加密根，以及不变的非加密/冷启动。这项认证回归不执行真实休眠。
- `tools/test-install.py`：安装程序的建议、`--secure-boot` 检查、fwupd 报告，以及顺序（密钥先于映像生成，映像上了 ESP 之后才登记；然后 prepare，或密钥没登记进去时用 PCR 0+7 绑定）。
- `tools/test-installer.c`：设置模式下的安全启动选项、参数、HSI 及其未通过的检查项。
- `tools/test-settings-monitors.c`：各行的顺序；"安全"页显示 TPM、安全启动和 HSI，且只向 fwupd 询问一次。
- `tools/test-settings-monitors.c` 还用虚拟机中 `locachy-tpm status --json` 的输出驱动 TPM 和安全启动两行：TPM 1.2 已绑定、已清除、已关闭；TPM 2.0 使用自有密钥；有绑定在等待。
- 在带 swtpm 和 OVMF 的虚拟机中（先是带安全启动的 TPM 2.0，先处于设置模式；再把 swtpm 作为 TPM 1.2）：运行记录及日志见 REGRESSIONS。
  - 使用 TPM 1.2 时，换用另一个 OVMF 构建后 PCR 0 改变；另一个构建写过变量存储之后，即使换回同一构建 PCR 0 也会改变。两种情况下 TPM 都拒绝，口令都能打开磁盘。
  - q35 芯片组的 TCO 看门狗会在从休眠恢复约 30 秒后复位虚拟机：恢复后的内核里的 iTCO_wdt 没有接管它，启动路径中也没有任何东西加载它。因此休眠测试使用 `-global ICH9-LPC.noreboot=true`。这是虚拟机芯片组的问题：无论 HEAD 还是本树，1 号进程都不碰看门狗（HEAD 未在虚拟机中经历过恢复）。在 X230 上没有见过。
