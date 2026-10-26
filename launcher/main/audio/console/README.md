# console

The codec verb, registered in the console's verb table from here, beside the codec it drives.

`codec <reg> [value]` reads a codec register, or writes it before reading it
back. Arguments are decimal or `0x` hexadecimal bytes. The development-only
verb requires an open codec and replies `CODEC reg=0xNN value=0xNN` on success.
