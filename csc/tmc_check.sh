#!/bin/bash

# Base address
CS_BASE=0xfe800000

# TMC offsets
declare -A TMC_OFFSETS=(
    [1]=0x140000
    [2]=0x150000
    [3]=0x170000
)

# Define registers and their offsets
declare -A REGISTERS=(
    ["STATUS"]=0xc
    ["RRD"]=0x10
    ["RRP"]=0x14
    ["RWP"]=0x18
    ["CTL"]=0x20
    ["RRPHI"]=0x38
    ["RWPHI"]=0x3C
    ["FFSR"]=0x300
    ["FFCR"]=0x304
)

# Function to read and print register value
read_register() {
    local tmc_num=$1
    local reg_name=$2
    local tmc_offset=${TMC_OFFSETS[$tmc_num]}
    local reg_offset=${REGISTERS[$reg_name]}
    local address=$((CS_BASE + tmc_offset + reg_offset))
    
    echo "Checking TMC$tmc_num $reg_name (offset 0x$(printf '%x' $reg_offset)):"
    value=$(./devmem $address)
    echo "  Value: $value"
    echo
}

# Function to print usage
print_usage() {
    echo "Usage: $0 <TMC_NUMBER> <REGISTER_NAME>"
    echo "Available TMC numbers: 1, 2, 3"
    echo "Available registers:"
    for reg in "${!REGISTERS[@]}"; do
        echo "  $reg"
    done
}

# Main script
if [ $# -ne 2 ]; then
    print_usage
    exit 1
fi

tmc_num=$1
reg_name=$2

if [[ ! ${TMC_OFFSETS[$tmc_num]} ]]; then
    echo "Error: Invalid TMC number. Choose 1, 2, or 3."
    print_usage
    exit 1
fi

if [[ ! ${REGISTERS[$reg_name]} ]]; then
    echo "Error: Invalid register name."
    print_usage
    exit 1
fi

echo "TMC$tmc_num Register Check"
echo "======================"
echo

read_register $tmc_num $reg_name

echo "Register check complete."