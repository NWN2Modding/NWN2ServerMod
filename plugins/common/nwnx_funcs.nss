// nwnx_funcs.nss - the script side of xp_funcs.
//
// Original: NWNX4's src/plugins/xp_funcs/nwnx_funcs.nss
// Copyright (C) 2010 Andrew Brockert (Zebranky), GPL v2 or later.
//
// GetCreatureSoundSet has the same signature and the same meaning as NWNX4's. What changed is
// underneath: the plugin no longer reaches into the engine to find the creature. It asks the
// engine to serialize it through SCORCO and reads SoundSetFile out of the GFF, which needs no
// engine addresses and so works unchanged on both the GOG and Steam builds.
//
// The cost is that the creature is serialized on every call - tens of KB. Fine for the occasional
// soundset lookup, bad in a loop over every creature in an area. Cache the result if you need it
// repeatedly for the same creature.

const string FUNCS_PLUGIN = "FUNCS";

// Returns the soundset ID of a given creature, as listed in soundset.2da.
// Returns 0 if the creature could not be read.
int GetCreatureSoundSet(object oCreature);

// Returns any integer field of a creature by its GFF label, e.g. "Appearance_Type", "Gender",
// "FactionID". Returns nDefault if the object could not be read or the field is not an integer.
//
// The labels are the ones in the .UTC blueprint, which the toolset and any GFF editor will show.
int GetCreatureFieldInt(object oCreature, string sLabel, int nDefault = 0);

int GetCreatureSoundSet(object oCreature)
{
    return GetCreatureFieldInt(oCreature, "SoundSetFile", 0);
}

int GetCreatureFieldInt(object oCreature, string sLabel, int nDefault = 0)
{
    // The store hands the plugin the serialized creature; the get reads one field back out of it.
    if (!StoreCampaignObject(FUNCS_PLUGIN, "field", oCreature))
    {
        return nDefault;
    }

    // NWNXGetInt cannot distinguish "no value" from a genuine 0, so a missing field and a field
    // that really is 0 both arrive here as 0. That matches what the original did.
    return NWNXGetInt(FUNCS_PLUGIN, "GETFIELDINT", sLabel, 0);
}
