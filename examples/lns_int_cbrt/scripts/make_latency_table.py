#!/usr/bin/env python3
"""Render results/latency_assumptions.csv as plots/latency_assumptions_table.png.
Usage: python3 scripts/make_latency_table.py results/latency_assumptions.csv plots/latency_assumptions_table.png"""
import csv, textwrap, sys
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
rows=list(csv.DictReader(open(sys.argv[1])))
cols=[("Instruction","instruction",28),("Used by","used_by",20),("Latency\n(cycles)","latency_cycles",9),
      ("Overlapped","overlapped_cycles",10),("Where the number comes from","where_the_number_comes_from",52),("Guess?","guess",20)]
cell=[[textwrap.fill(r[k],w) for _,k,w in cols] for r in rows]
nl=[max(c.count("\n")+1 for c in row) for row in cell]
fig_h=0.27*(sum(nl)+2)+0.5
fig,ax=plt.subplots(figsize=(17,fig_h)); ax.axis("off")
t=ax.table(cellText=cell,colLabels=[c for c,_,_ in cols],loc="upper center",cellLoc="left",
           colWidths=[w/139 for _,_,w in cols])
t.auto_set_font_size(False); t.set_fontsize(10)
for (i,j),c in t.get_celld().items():
    lines = 2 if i==0 else nl[i-1]
    c.set_height(0.27*lines/fig_h)
    c.set_edgecolor("#bbbbbb")
    if i==0: c.set_facecolor("#2f3b4c"); c.get_text().set_color("white"); c.get_text().set_fontweight("bold")
    else:
        g=rows[i-1]["guess"]
        if j==5: c.set_facecolor("#e8f5e9" if g.startswith("From paper") else ("#fff3e0" if "Guess" in g or "Derived" in g else "#f5f5f5"))
        if j in (2,3): c.get_text().set_ha("center"); c.get_text().set_fontweight("bold" if j==2 else "normal")
ax.set_title("Cube-root time estimate: one fixed latency per instruction (ESTIMATE inputs)",fontsize=13,fontweight="bold",loc="left")
fig.text(0.01,0.005,"Green = from Parhami (2020) Table 3. Orange = guess or derived. Sensitivity of the guesses: RESULTS.md I.F and plots/7b_sensitivity.png.",fontsize=9,color="#555")
plt.savefig(sys.argv[2],dpi=200,bbox_inches="tight"); print("ok")
