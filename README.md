# UARTControllerNX
シリアル接続で制御可能なNintendo Switch用コントローラをESP32でエミュレートします。

ESP32はBluetooth接続によってPro Controllerとして振る舞い、Nintendo Switchに対して各ボタンやスティック入力の機能を提供します。  
PCからシリアル通信でコントローラ状態を受け取ることを想定した作りになっています。

# シリアル通信の仕様

[ぼんじりさん作のNX Macro Controller v2](https://blog.bzl-web.com/entry/2020/12/13/204230) の出力を受けられるようにしています。  
伝送データフォーマットは公開されていなかったため、独自に解析した結果から想定しています。

通信は一般的なシリアル通信です。USBシリアル接続でUART0を使用します。

- ボーレート 9600bps
- 8ビット
- ストップビット1
- パリティなし

## 伝送データ

ESP32はNintendo Switchとの接続が成功すると60Hzでコントローラ状態を送信します。  
PCから伝送データを受けると、コントローラ状態に反映して次回以降の送信に使います。

伝送データは11バイトで、コントローラのすべての入力状態をまとめたデータです。  
Byte0-4と10は固定で、入力状態による変化はありません。

| Byte | 0 - 4       | 5       | 6       | 7    | 8       | 9       | 10          |
|------|-------------|---------|---------|------|---------|---------|-------------|
| Data | Flag (0xAA) | Button0 | Button1 | DPad | L Stick | R Stick | Flag (0x00) |

### Byte 5: Button0 (ボタン0)

| Bit    | 0 | 1 | 2 | 3 | 4 | 5 | 6  | 7  |
|--------|---|---|---|---|---|---|----|----|
| Button | Y | B | A | X | L | R | ZL | ZR |


### Byte 6: Button1 (ボタン1)

| Bit    | 0     | 1    | 2     | 3     | 4    | 5       | 6      | 7      |
|--------|-------|------|-------|-------|------|---------|--------|--------|
| Button | Minus | Plus | L Clk | R Clk | Home | Capture | 0 (SL) | 0 (SR) |

### Byte 7: DPad (十字キー)

	A_DPAD_CENTER    = 0x08
	A_DPAD_U         = 0x00
	A_DPAD_U_R       = 0x01
	A_DPAD_R         = 0x02
	A_DPAD_D_R       = 0x03
	A_DPAD_D         = 0x04
	A_DPAD_D_L       = 0x05
	A_DPAD_L         = 0x06
	A_DPAD_U_L       = 0x07

真上から見たときの方向と値:

| -- | TOP | -- |
| --:|:---:|:-- |
| 7  |  0  |  1 |
| 6  |  8  |  2 |
| 5  |  4  |  3 |
	
### Byte 8, 9: Stick (スティック入力)

|Input |      |
|------|------|
|Left  | 0x01 |
|Right | 0x02 |
|Up    | 0x04 |
|Down  | 0x08 |

真上から見たときの方向と値:

| --   | TOP  | --   |
|-----:|:----:|:-----|
| 0x05 | 0x04 | 0x06 |
| 0x01 | 0x00 | 0x02 |
| 0x09 | 0x08 | 0x0A |

# おわりに

このプログラムの使用について、NX Macro Controllerの作者であるぼんじりさんや、他のソフトウェア・ツール・ユーティリティの作者様に問い合わせることは固くご遠慮ください。

# Thanks

多くの情報を参考にさせていただきました、ありがとうございます。

- [NathanReeves/BlueCubeMod](https://github.com/NathanReeves/BlueCubeMod).
- [wchill/SwitchInputEmulator](https://github.com/wchill/SwitchInputEmulator).
- [nullstalgia/UARTSwitchCon](https://github.com/nullstalgia/UARTSwitchCon), and v12 Switch Firmware support [mizuyoukan-ao](https://github.com/mizuyoukanao).