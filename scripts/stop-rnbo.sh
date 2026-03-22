#!/bin/bash
echo "Stopping RNBO stack..."
pkill -f rnbo-runner-panel 2>/dev/null
pkill -f jack_transport_link 2>/dev/null
pkill -f rnbooscquery 2>/dev/null
pkill -f "jackd.*shadow" 2>/dev/null
echo "Done."
