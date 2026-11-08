# workshop_fastload

《求生之路2》的 left4neko（l4n）插件，用来缩短每次启动游戏时的创意工坊检查。

## 效果

| | 检查 180 个订阅 mod 的耗时 |
| --- | --- |
| 不装插件 | 约 55 秒 |
| 装插件后 | 约 7 秒 |

这是在一台机器上的测试结果，实际时间和订阅数量、网络情况有关。订阅越多，省下的时间越多。

插件只加快启动时的检查，进地图的速度不变。

## 原理

游戏启动时会向 Steam 逐个询问订阅的 mod 有没有更新。询问时用的是 `GetPublishedFileDetails`，参数 `unMaxSecondsOld` 为 0，要求 Steam 每次都去服务器查。每个 mod 大约要 0.3 秒，而且要等上一个查完才查下一个，订阅多了就要等很久。

Steam 在本机保存了每个 mod 上次查到的信息。插件在游戏发出询问时把这个参数改成一个很大的值，Steam 就直接用本机的信息回答，不用再访问服务器。

插件通过 l4n 的插件接口加载，不修改任何游戏文件。

## 安装

需要先安装 left4neko（本插件基于 2.51 版本开发）。

1. 在 [Releases](https://github.com/Fisnetia/workshop_fastload/releases) 下载 `workshop_fastload.dll`。
2. 把它直接放进 `Left 4 Dead 2\bin\neko\plugins\`，不要放进子文件夹。
3. 启动游戏。第一次启动时，同一个文件夹里会自动生成配置文件 `workshop_fastload.ini`。

插件会修改游戏进程的内存，不确保 VAC 的安全性，建议在启动项里加上 `-insecure`。

卸载时删除 `plugins` 文件夹里的 `workshop_fastload.dll`、`workshop_fastload.ini` 和 `workshop_fastload.log`。

## 使用

装好后不需要额外设置。

插件默认不再检查普通 mod 的更新，left4neko 和 skeeto 除外，这两个每次启动都会照常检查。新订阅的 mod 不受影响，可以正常使用。

### 更新 mod

打开 l4n 菜单，进入“插件 → 创意工坊Mod检查”，把“下次启动检查全部 mod 更新”切换为“开”。

下一次启动时，游戏会检查所有订阅的 mod，并下载有更新的 mod。这次启动大约会多花一分钟，之后开关自动恢复为“关”。

### 让某些 mod 每次都检查更新

打开 `workshop_fastload.ini`，去掉 `bypass_ids` 这一行开头的分号，填上 mod 的 ID，多个 ID 用逗号隔开：

```ini
bypass_ids=1234567890,2345678901
```

mod 的 ID 是创意工坊网址中 `?id=` 后面的数字，也是 `addons\workshop` 文件夹里 `.vpk` 文件的文件名。数量没有限制，每加一个，启动大约多花 0.3 秒。

### 配置项

配置文件是 `workshop_fastload.ini`，修改后下次启动生效。把它删掉，下次启动会重新生成默认配置。

| 配置项 | 说明 |
| --- | --- |
| `mode` | `1` 为加速（默认）；`0` 为只记录，不改变游戏行为 |
| `max_seconds_old` | 本机信息的有效期。`never` 为一直有效（默认），也可以填 60 以上的秒数，例如 `604800` 是 7 天。超过有效期的 mod 会重新检查 |
| `check_all_next_launch` | 设为 `1` 时，下次启动检查全部 mod，效果和菜单开关相同 |
| `bypass_ids` | 每次都检查更新的 mod ID |

### 查看运行情况

每次启动后，`plugins` 文件夹里会生成 `workshop_fastload.log`，每个 mod 对应一行：

```
GPD    id=469986973    maxSecondsOld=0          -> 2147483647
GPD    id=3558049615   maxSecondsOld=0          -> 0           (keep, tooling)
```

箭头后面是很大的数字，表示这个 mod 使用本机信息；是 `0`，表示这个 mod 照常检查了更新。

如果没有生成这个文件，说明插件没有被 l4n 加载，请检查文件位置和 l4n 版本。
