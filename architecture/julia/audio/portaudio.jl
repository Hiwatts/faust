# ************************************************************************
# FAUST Architecture File
# Copyright (C) 2021 GRAME, Centre National de Creation Musicale
# ---------------------------------------------------------------------

# This is sample code. This file is provided as an example of minimal
# FAUST architecture file. Redistribution and use in source and binary
# forms, with or without modification, in part or in full are permitted.
# In particular you can create a derived work of this FAUST architecture
# and distribute that work under terms of your choice.

# This sample code is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# ************************************************************************

const FAUSTFLOAT = Float32

include("/usr/local/share/faust/julia/gui/MapUI.jl")

# Generated code
<<includeIntrinsic>>
<<includeclass>>

# Testing
samplerate = Int32(44100)
block_size = Int32(512)

# Using PortAudio
using PortAudio

devices = PortAudio.devices()
#dev = filter(x -> x.maxinchans == 2 && x.maxoutchans == 2, devices)[1]

# Selecting a Duplex device here
#dev = devices[10]

#PortAudioStream(dev, dev) do stream
PortAudioStream(1, 2) do stream
    dsp = mydsp()
    map_ui = MapUI(dsp)
    println("getNumInputs ", getNumInputs(dsp))
    println("getNumOutputs ", getNumOutputs(dsp))
    init(dsp, samplerate)
    buildUserInterface(dsp, map_ui)
    # Print all paths
    println(getMap(map_ui))
    # Possibly change control values
    #setParamValue(map_ui, "/Oscillator/freq", 500.0f0)
    #setParamValue(map_ui, "/Oscillator/volume", -10.0f0)
    outputs = zeros(REAL, block_size, getNumOutputs(dsp))
    while true
        inputs = convert(Matrix{REAL}, read(stream, block_size))
        compute(dsp, block_size, inputs, outputs)
        write(stream, outputs)
    end
end