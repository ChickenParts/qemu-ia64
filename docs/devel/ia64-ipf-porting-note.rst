IA-64 legacy IPF.c porting note
================================

This branch audits the historical IPF.c machine implementation as a migration
ledger.  Legacy source is not a behavioural specification: architectural and
chipset contracts remain authoritative, and firmware-specific conditionals,
magic program-counter checks, synthetic success returns, and timing delays are
not acceptable substitutes for root-cause fixes.
