# In a session

`autana` alone starts a session: the same commands without the prefix. Any
other line goes to the board as typed, so an app's own command works
([App-Commands.md](App-Commands.md)). `use A308` pins a board; `use -` clears it.

```text
autana> tune theme_rgb 0x1199C8
autana> debug freeze
autana> debug step 3
autana> debug resume
autana> quit
```

A session takes the board's lock for each line and lets go, so the board stays
free between two of them ([Device-Lock.md](Device-Lock.md)).
