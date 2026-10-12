"""Binary byte units and compact size labels shared by reports and charts."""

KB_BYTES = 1024
MB_BYTES = KB_BYTES * KB_BYTES


def size_label(nbytes):
    unit, name = (MB_BYTES, "MB") if nbytes >= MB_BYTES else (KB_BYTES, "KB")
    return f"{nbytes // unit} {name}" if nbytes % unit == 0 else f"{nbytes / unit:.1f} {name}"
