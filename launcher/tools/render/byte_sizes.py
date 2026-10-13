"""Binary byte units and compact size labels shared by reports and charts."""

KB_BYTES = 1024
MB_BYTES = KB_BYTES * KB_BYTES


def size_label(nbytes):
    unit, name = (MB_BYTES, "MB") if nbytes >= MB_BYTES else (KB_BYTES, "KB")
    return f"{nbytes // unit} {name}" if nbytes % unit == 0 else f"{nbytes / unit:.1f} {name}"


def share_label(part, total):
    """`part` against its ceiling `total`, the unit written once when both share it."""
    part_value, part_unit = size_label(part).split()
    whole = size_label(total)
    return f"{part_value} / {whole}" if whole.endswith(" " + part_unit) else f"{size_label(part)} / {whole}"


def total_label(nbytes):
    """A ceiling in exact bytes and its compact size."""
    return f"{nbytes:,} B = {size_label(nbytes)}"
