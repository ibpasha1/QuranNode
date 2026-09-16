#!/usr/bin/env python3
"""Import FreeRouting's qurannode.ses back into qurannode.kicad_pcb, save, and report
routing completion. Run with KiCad's bundled python."""
import os, pcbnew
HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, "qurannode.kicad_pcb")
SES = os.path.join(HERE, "qurannode.ses")

board = pcbnew.LoadBoard(PCB)
try:
    ok = pcbnew.ImportSpecctraSES(board, SES)
except TypeError:
    ok = pcbnew.ImportSpecctraSES(SES)
print("SES import:", ok)

pcbnew.SaveBoard(PCB, board)

# report
tracks = [t for t in board.GetTracks() if t.Type() == pcbnew.PCB_TRACE_T]
vias   = [t for t in board.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
board.BuildConnectivity()
conn = board.GetConnectivity()
try:
    unrouted = conn.GetUnconnectedCount(True)
except Exception:
    unrouted = "?"
print(f"tracks: {len(tracks)}  vias: {len(vias)}  unrouted ratlines: {unrouted}")
