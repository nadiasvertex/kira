#!/bin/bash
container build -t local/ubuntu-machine:latest .
container machine rm ubuntu
container machine create local/ubuntu-machine:latest --name ubuntu
