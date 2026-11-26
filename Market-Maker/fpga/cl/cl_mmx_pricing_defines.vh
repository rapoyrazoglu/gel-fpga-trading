// ============================================================================
// Amazon FPGA Hardware Development Kit
//
// Copyright 2026 Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Amazon Software License (the "License"). You may not use
// this file except in compliance with the License. A copy of the License is
// located at
//
//    http://aws.amazon.com/asl/
//
// or in the "license" file accompanying this file. This file is distributed on
// an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, express or
// implied. See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================================

`ifndef CL_AXIL_REG_ACCESS_DEFINES
`define CL_AXIL_REG_ACCESS_DEFINES

//Put module name of the CL design here.  This is used to instantiate in top.sv
`define CL_NAME cl_axil_reg_access

// Uncomment to disable ILA debug
// `define NO_CL_AXIL_DEBUG_ILA

// MMX smart-pricing register map
`define ADDR_S              32'h00
`define ADDR_Q              32'h04
`define ADDR_INV_SKEW       32'h08
`define ADDR_HALF_SPREAD    32'h0C
`define ADDR_MAX_INV        32'h10
`define ADDR_BASE_QTY       32'h14
`define ADDR_SKEWS          32'h18
`define ADDR_KILL           32'h1C
`define ADDR_CONTROL_STATUS 32'h20
`define ADDR_BID            32'h24
`define ADDR_ASK            32'h28
`define ADDR_QTY            32'h2C
`define ADDR_CANCEL         32'h30
`define ADDR_IMBALANCE      32'h34
`define ADDR_OBI_GAIN       32'h38
`define ADDR_K_SHIFT        32'h3C
`define ADDR_VOL_GAIN       32'h40
`define ADDR_PNL            32'h44
`define ADDR_CAP_THRESH     32'h48
`define ADDR_PNL_DROP       32'h4C
`define ADDR_HOST_RESET     32'h50
`define ADDR_SPREAD_ADD     32'h54
`define ADDR_VOL_OUT        32'h58
`define ADDR_HARD_KILL      32'h5C
`define ADDR_TRIP           32'h60
`define ADDR_LATENCY        32'h64
`define INVALID_ADDR_RESP   32'hDEADBEEF

// AXI constants
`define AXI_PROT_DEFAULT    3'h0
`define AXI_RESP_OKAY       2'b00

`endif
