RenkuOS test image -- AirOS fixes, built from the X270 test branch.

Four tests live in /boot/home/tests. Run them from Terminal and read their
RESULT lines in SYSLOG.TXT on the EFI (FAT) partition afterwards:

  xsi_sem_number_test          semctl() with a semaphore number one past the set
  kqueue_unsupported_fd_test   kqueue event registration on an unsupported fd
  msync_large_offset_test      msync() 4 GiB or more into a file
  syslog_long_message_test     syslog() with a message longer than its buffer

The "fixed" image contains the four fixes; the "baseline" image contains only
the tests. SYSLOG.TXT is rewritten by the kernel every 10 seconds with the
kernel syslog and the tail of /boot/system/var/log/syslog.

Write the .img to a USB stick with dd (Rufus: "DD image") and boot it on an
x86_64 UEFI machine.
