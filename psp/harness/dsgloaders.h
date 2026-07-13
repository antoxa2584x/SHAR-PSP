//=============================================================================
// dsgloaders.h — SRR2 DSG chunk unwrappers for the PSP harness.
//
// A SHAR level .p3d stores its world geometry in game-specific "DSG" (Dynamic
// Scene Graph) chunks that WRAP a Pure3D MESH:
//   ENTITY_DSG        (0x03f00000) : name, version, hasAlpha, then a MESH
//   WORLD_SPHERE_DSG  (0x03f0000b) : name, version, numMeshes, numBBQG, MESHes
//
// These loaders build the game's REAL render objects from those chunks:
//   * ENTITY_DSG      -> a real StaticEntityDSG (src/game/render/DSG), which
//                        wraps the tGeometry, runs ProcessShaders, and carries
//                        the translucency flag + SetRank() ranking the game uses.
//   * WORLD_SPHERE_DSG-> the sky/backdrop meshes, kept in a separate list so the
//                        harness can draw them FIRST with depth-write disabled
//                        (as WorldRenderLayer does — the sky is not in the tree).
//
// The full WorldScene spatial-tree culling layer is NOT ported (its Add/Place
// bodies pull StaticPhysDSG/collisionentitydsg + worldsim/physics — the sim
// cascade). Instead the harness drives these real DSG entities directly with
// the game's render ORDERING: sky (Z-write off) -> opaque -> depth-sorted
// translucent. Per-mesh frustum culling already lives in tGeometry::Display.
//=============================================================================
#ifndef PSP_HARNESS_DSGLOADERS_H
#define PSP_HARNESS_DSGLOADERS_H

#include <p3d/loadmanager.hpp>   // tSimpleChunkHandler, tEntityStore
#include <p3d/chunkfile.hpp>
#include <p3d/geometry.hpp>      // tGeometry, tGeometryLoader
#include <render/DSG/StaticEntityDSG.h>

// SRR2 DSG chunk ids (see src/game/constants/srrchunks.h).
static const unsigned PSP_ENTITY_DSG        = 0x03f00000;
static const unsigned PSP_WORLD_SPHERE_DSG  = 0x03f0000b;
static const unsigned PSP_MESH_CHUNK        = 0x00010000;   // Pure3D::Mesh::MESH

// Harness sinks (defined in psp/harness/main.cpp). The loaders push the real
// render objects into the harness's scene lists; the harness owns their refs.
void PspAddWorldEntity( StaticEntityDSG* dsg );   // one ENTITY_DSG static entity
void PspAddSkyGeo( tGeometry* geo );              // one WORLD_SPHERE_DSG mesh

//-----------------------------------------------------------------------------
// ENTITY_DSG -> real StaticEntityDSG (world-space static level geometry).
//-----------------------------------------------------------------------------
class PspEntityDSGLoader : public tSimpleChunkHandler
{
public:
    PspEntityDSGLoader( tGeometryLoader* geoLoader )
        : tSimpleChunkHandler( PSP_ENTITY_DSG ), mGeoLoader( geoLoader ) {}

    virtual tEntity* LoadObject( tChunkFile* f, tEntityStore* store )
    {
        char name[256];
        f->GetPString( name );
        f->GetLong();                    // version
        int hasAlpha = f->GetLong();     // translucent flag

        tGeometry* geo = NULL;
        while ( f->ChunksRemaining() )
        {
            f->BeginChunk();
            if ( f->GetCurrentID() == PSP_MESH_CHUNK )
                geo = (tGeometry*) mGeoLoader->LoadObject( f, store );
            f->EndChunk();
        }

        if ( geo != NULL )
        {
            StaticEntityDSG* dsg = new StaticEntityDSG;
            dsg->AddRef();
            dsg->SetName( name );
            dsg->SetGeometry( geo );          // AddRefs geo, runs ProcessShaders
            if ( hasAlpha ) dsg->mTranslucent = true;
            PspAddWorldEntity( dsg );         // harness holds the ref
        }
        // We own the entity via the harness list, so nothing to store here.
        return NULL;
    }

private:
    tGeometryLoader* mGeoLoader;
};

//-----------------------------------------------------------------------------
// WORLD_SPHERE_DSG -> sky/backdrop meshes (drawn first, depth-write off).
//-----------------------------------------------------------------------------
class PspWorldSphereLoader : public tSimpleChunkHandler
{
public:
    PspWorldSphereLoader( tGeometryLoader* geoLoader )
        : tSimpleChunkHandler( PSP_WORLD_SPHERE_DSG ), mGeoLoader( geoLoader ) {}

    virtual tEntity* LoadObject( tChunkFile* f, tEntityStore* store )
    {
        char name[256];
        f->GetPString( name );
        f->GetLong();   // version
        f->GetLong();   // numMeshes
        f->GetLong();   // numBillboardQuadGroups

        while ( f->ChunksRemaining() )
        {
            f->BeginChunk();
            if ( f->GetCurrentID() == PSP_MESH_CHUNK )
            {
                tGeometry* geo = (tGeometry*) mGeoLoader->LoadObject( f, store );
                if ( geo != NULL )
                {
                    geo->AddRef();
                    PspAddSkyGeo( geo );
                }
            }
            f->EndChunk();
        }
        return NULL;
    }

private:
    tGeometryLoader* mGeoLoader;
};

#endif // PSP_HARNESS_DSGLOADERS_H
