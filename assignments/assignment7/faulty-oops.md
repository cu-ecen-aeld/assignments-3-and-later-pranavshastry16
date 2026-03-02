# Assignment 7: Faulty Kernel Oops Analysis

## After booting my Buildroot QEMU image, I verified the `faulty` module was loaded and then triggered the failure using:

```sh
echo "hello_world" > /dev/faulty
