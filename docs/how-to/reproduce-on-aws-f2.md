# Reproduce the result on AWS F2

This puts the data plane on a real VU47P and reads back the latency counter.
It costs money: an f2.6xlarge is roughly 2 USD per hour, and building an FPGA
image takes one to two hours.

## You need

- An AWS account with F-family vCPU quota (new accounts often have zero).
- An f2.6xlarge running the FPGA Developer AMI with Vivado 2025.2.
- On the instance: the `aws-fpga` repository, **`f2` branch**, with
  `hdk_setup.sh` and `sdk_setup.sh` sourced.
- IAM permission for `ec2:CreateFpgaImage` and an S3 bucket in the same region.

## 1. Copy the design to the instance

From your workstation:

```sh
F2_KEY=~/.ssh/my-key.pem F2_HOST=ubuntu@<instance-dns> \
  Market-Maker/fpga/cl_dataplane/deploy_dataplane.sh
```

The script copies the custom-logic top, its register-map header and the twelve
VHDL files into the HDK's `cl_axil_reg_access` example, and patches that
example's build scripts to read VHDL-2008. It backs up the originals once and
can be re-run. It does not start a build.

## 2. Build, create the image, load it

On the instance, inside the example's `build/scripts` directory, follow the
standard F2 flow: build the design checkpoint with the HDK's
`aws_build_dcp_from_cl` script, create the image with
`aws ec2 create-fpga-image`, wait until it is `available`, then load it with
`fpga-load-local-image`.

Check the timing report before creating the image. The shipped design closes
at +0.102 ns of worst negative slack.

## 3. Run the driver

[`mmx_dataplane_test.cpp`](../../Market-Maker/fpga/cl_dataplane/mmx_dataplane_test.cpp)
loads a signed parameter block, injects one market-data frame, reads the order
frame back and prints the latency counter. It is C++ and must be linked with
`host/ipc/hmac.cpp` and `host/ipc/sha256.cpp` and the AWS FPGA management
library. The header comment in the file describes each step.

The expected latency is 20 cycles.

## What differs from the simulation

The silicon build leaves out the HMAC check on the parameter block. See
[timing closure](../explanation/timing-closure.md) for why.

The registers the driver uses are listed in the
[register map](../reference/register-map.md).
