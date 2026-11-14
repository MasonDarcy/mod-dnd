/*
 * mod-dnd placement: checks that every encounter spawn stands somewhere sensible, using the server's own collision
 * data, and finds a better spot nearby when it doesn't.
 *
 * A spot is good when
 *   - it is on the walkable navmesh (the mesh has holes around tree trunks, boulders and cliffs, so a spawn that is
 *     more than a little way from it is inside something),
 *   - it is not boxed in (sight lines from a ring of points around it, at chest height, mostly reach it), and
 *   - the ground around it is not too steep.
 * `.dnd spawncheck` reports; `.dnd spawncheck fix` also moves failing spawns to the nearest good spot that keeps the
 * group together, and writes the corrections to logs/dnd_spawnfix.json (tools/apply_spawnfix.py folds them into the
 * encounter data so they survive the next regeneration).
 */
#include "Dnd.h"

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "StringFormat.h"
#include "VMapDefinitions.h"
#include <cmath>
#include <fstream>

namespace Dnd
{
    struct SpotCheck
    {
        bool ok = false;
        float z = 0.f;
        float meshDist = 99.f;       // horizontal distance to the walkable mesh
        int blocked = 0;             // of 8 sight lines from around the spot
        float slope = 0.f;           // biggest height step to ground 1 m away
        std::string why;
    };

    static float Ground(Map* map, float x, float y, float z)
    {
        return map->GetHeight(PHASEMASK_NORMAL, x, y, z + 4.f, true, 15.f);
    }

    static float MeshDistance(Map* map, float x, float y, float z)
    {
        dtNavMeshQuery const* q = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!q)
            return 0.f;                  // no navmesh for this map: can't tell
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(0);
        float point[3] = { y, z, x };    // Recast works in y, z, x
        float extents[3] = { 3.f, 4.f, 3.f };
        float closest[3] = { 0.f, 0.f, 0.f };
        dtPolyRef ref = 0;
        if (!dtStatusSucceed(q->findNearestPoly(point, extents, &filter, &ref, closest)) || !ref)
            return 99.f;
        return std::sqrt((closest[0] - y) * (closest[0] - y) + (closest[2] - x) * (closest[2] - x));
    }

    static SpotCheck TestSpot(Map* map, float x, float y, float zGuess)
    {
        SpotCheck r;
        float g = Ground(map, x, y, zGuess);
        if (g <= INVALID_HEIGHT || std::fabs(g - zGuess) > 12.f)
        {
            r.why = "no ground";
            return r;
        }
        r.z = g;

        // steepness
        for (int i = 0; i < 8; ++i)
        {
            float a = i * float(M_PI) / 4.f;
            float h = Ground(map, x + std::cos(a), y + std::sin(a), g);
            if (h > INVALID_HEIGHT)
                r.slope = std::max(r.slope, std::fabs(h - g));
        }

        // on the walkable mesh, with room around it (a body is wider than a point)
        r.meshDist = MeshDistance(map, x, y, g);
        for (int i = 0; i < 4; ++i)
        {
            float a = i * float(M_PI) / 2.f + float(M_PI) / 4.f;
            float nx = x + std::cos(a) * 0.8f, ny = y + std::sin(a) * 0.8f;
            float nz = Ground(map, nx, ny, g);
            r.meshDist = std::max(r.meshDist, MeshDistance(map, nx, ny, nz > INVALID_HEIGHT ? nz : g));
        }

        // boxed in? sight lines from 2.5 m around, chest height to chest height
        for (int i = 0; i < 8; ++i)
        {
            float a = i * float(M_PI) / 4.f;
            float ox = x + std::cos(a) * 2.5f, oy = y + std::sin(a) * 2.5f;
            float oz = Ground(map, ox, oy, g);
            if (oz <= INVALID_HEIGHT)
                oz = g;
            if (!map->isInLineOfSight(ox, oy, oz + 1.2f, x, y, g + 1.2f, PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS,
                VMAP::ModelIgnoreFlags::Nothing))
                ++r.blocked;
        }

        if (r.meshDist > 0.35f)
            r.why = Acore::StringFormat("too close to an obstacle ({:.1f} m off the walkable mesh - a tree, rock or cliff)", r.meshDist);
        else if (r.blocked >= 2)
            r.why = Acore::StringFormat("boxed in ({}/8 sight lines blocked)", r.blocked);
        else if (r.slope > 0.9f)
            r.why = Acore::StringFormat("on a steep slope ({:.1f} m drop within 1 m)", r.slope);
        else
            r.ok = true;
        return r;
    }

    struct Fix { uint32 guid; float x, y, z; };

    void CheckSpawns(ChatHandler* handler, bool fix)
    {
        std::vector<Fix> fixes;
        uint32 good = 0, bad = 0, moved = 0;
        for (EncounterDef const* def : AllEncounters())
        {
            std::vector<Position> placed;    // where this group's members stand (after fixes)
            Position leader;
            for (std::size_t n = 0; n < def->spawns.size(); ++n)
            {
                uint32 guid = def->spawns[n];
                CreatureData const* data = sObjectMgr->GetCreatureData(guid);
                if (!data)
                    continue;
                Map* map = sMapMgr->CreateBaseMap(data->mapid);
                if (!map)
                    continue;
                map->LoadGrid(data->posX, data->posY);
                SpotCheck c = TestSpot(map, data->posX, data->posY, data->posZ);
                Position at(data->posX, data->posY, c.ok ? c.z : data->posZ, data->orientation);
                bool crowded = false;
                for (Position const& o : placed)
                    if (o.GetExactDist2d(&at) < 1.4f)
                        crowded = true;
                if (c.ok && !crowded)
                {
                    ++good;
                    placed.push_back(at);
                    if (n == 0)
                        leader = at;
                    continue;
                }
                ++bad;
                std::string why = c.ok ? "too close to another member" : c.why;
                handler->PSendSysMessage("[{}] {} spawn {} ({:.1f}, {:.1f}): {}", def->id, def->name, guid, data->posX, data->posY, why);
                if (!fix)
                {
                    placed.push_back(at);
                    continue;
                }
                // search outward for the nearest good spot that keeps the group together
                bool found = false;
                for (float r = 0.75f; r <= 12.f && !found; r += 0.75f)
                {
                    int steps = std::max(8, int(r * 6.f));
                    for (int i = 0; i < steps && !found; ++i)
                    {
                        float a = i * 2.f * float(M_PI) / steps;
                        float x = data->posX + std::cos(a) * r, y = data->posY + std::sin(a) * r;
                        SpotCheck t = TestSpot(map, x, y, data->posZ);
                        if (!t.ok)
                            continue;
                        Position cand(x, y, t.z, data->orientation);
                        bool keep = n == 0 || cand.GetExactDist2d(&leader) <= 14.f;
                        for (Position const& o : placed)
                            if (o.GetExactDist2d(&cand) < 1.6f)
                                keep = false;
                        if (!keep)
                            continue;
                        found = true;
                        fixes.push_back({ guid, x, y, t.z });
                        placed.push_back(cand);
                        if (n == 0)
                            leader = cand;
                        ++moved;
                        handler->PSendSysMessage("   -> moved {:.1f} m to ({:.1f}, {:.1f}, {:.1f})", r, x, y, t.z);
                        // live: put the creature there now, and in the world DB until the data is regenerated
                        WorldDatabase.Execute("UPDATE creature SET position_x = {}, position_y = {}, position_z = {} WHERE guid = {}",
                            x, y, t.z, guid);
                        auto range = map->GetCreatureBySpawnIdStore().equal_range(guid);
                        for (auto itr = range.first; itr != range.second; ++itr)
                        {
                            Creature* cr = itr->second;
                            cr->SetHomePosition(x, y, t.z, data->orientation);
                            if (cr->IsAlive() && !InCombat(cr))
                                cr->NearTeleportTo(x, y, t.z, data->orientation);
                        }
                    }
                }
                if (!found)
                {
                    handler->PSendSysMessage("   -> no good spot within 12 m - move this one by hand");
                    placed.push_back(at);
                }
            }
        }
        handler->PSendSysMessage("mod-dnd spawncheck: {} fine, {} with problems{}", good, bad,
            fix ? Acore::StringFormat(", {} moved", moved) : std::string());
        if (fix && !fixes.empty())
        {
            std::ofstream out("logs/dnd_spawnfix.json", std::ios::trunc);
            out << "{\n";
            for (std::size_t i = 0; i < fixes.size(); ++i)
                out << Acore::StringFormat("  \"{}\": [{:.2f}, {:.2f}, {:.2f}]{}\n", fixes[i].guid, fixes[i].x, fixes[i].y, fixes[i].z,
                    i + 1 < fixes.size() ? "," : "");
            out << "}\n";
            handler->PSendSysMessage("Corrections written to logs/dnd_spawnfix.json (python tools/apply_spawnfix.py keeps them).");
        }
    }

    // GM: where does the walking route from A to B cross from one zone/area into another, and how wide is the way
    // there? Used to place zone gates (tools/make_world_sql.py GATES). Path on the navmesh, sampled every metre.
    // A gate for a straight stretch of passage (from 1 to 2): every metre, the walls to either side (raycasts against the
    // world model at chest height) and the roof above; the narrowest steady spot wins. The portcullis model
    // (DWARVENTUNNELPORTCULLIS, display 401) is 6.94 m wide along its own X and 9.39 m tall: it faces across the way at
    // the path's heading + 90 degrees, centred between the walls, scaled to the smaller of width and roof.
    void FitGate(ChatHandler* handler, uint32 mapId, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        Map* map = sMapMgr->CreateBaseMap(mapId);
        if (!map)
            return;
        map->LoadGrid(x1, y1);
        map->LoadGrid(x2, y2);
        float dir = std::atan2(y2 - y1, x2 - x1);
        float len = std::hypot(x2 - x1, y2 - y1);
        auto clear = [&](float ax, float ay, float az, float bx, float by, float bz)
        {
            return map->isInLineOfSight(ax, ay, az, bx, by, bz, PHASEMASK_NORMAL, LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing);
        };
        struct Spot { float x, y, z, wl, wr, roof; };
        std::vector<Spot> spots;
        for (float d = 0.f; d <= len; d += 1.f)
        {
            float x = x1 + std::cos(dir) * d, y = y1 + std::sin(dir) * d;
            float z = Ground(map, x, y, z1 + (z2 - z1) * d / std::max(1.f, len) + 2.f);
            if (z <= INVALID_HEIGHT)
                continue;
            float w[2] = { 0.f, 0.f };
            for (int s = 0; s < 2; ++s)
            {
                float a = dir + (s ? -1.f : 1.f) * float(M_PI) / 2;
                for (float o = 0.25f; o <= 25.f; o += 0.25f)
                {
                    if (!clear(x, y, z + 1.5f, x + std::cos(a) * o, y + std::sin(a) * o, z + 1.5f))
                        break;
                    w[s] = o;
                }
            }
            float roof = 0.f;
            for (float h = 1.f; h <= 40.f; h += 0.25f)
            {
                if (!clear(x, y, z + 0.5f, x, y, z + h))
                    break;
                roof = h;
            }
            spots.push_back({ x, y, z, w[0], w[1], roof });
            handler->PSendSysMessage("  {:5.1f} m ({:.1f}, {:.1f}, {:.1f}): walls {:.1f} left + {:.1f} right = {:.1f} m, roof {:.1f} m",
                d, x, y, z, w[0], w[1], w[0] + w[1], roof);
        }
        // the best spot: a roof (not open sky), walls both sides, and the narrowest with its neighbours alike
        Spot const* best = nullptr;
        float bestScore = 1e9f;
        for (std::size_t i = 1; i + 1 < spots.size(); ++i)
        {
            Spot const& s = spots[i];
            float width = s.wl + s.wr;
            if (s.roof <= 0.f || s.roof >= 39.f || s.wl >= 24.f || s.wr >= 24.f || width < 2.f)
                continue;
            float steady = std::fabs(width - (spots[i - 1].wl + spots[i - 1].wr)) + std::fabs(width - (spots[i + 1].wl + spots[i + 1].wr));
            float score = width + steady;
            if (score < bestScore)
            {
                bestScore = score;
                best = &s;
            }
        }
        if (!best)
        {
            handler->SendSysMessage("gatefit: no spot with walls and a roof on that stretch");
            return;
        }
        float a = dir + float(M_PI) / 2;                 // the model's X axis spans the way
        float shift = (best->wl - best->wr) / 2;          // centre between the walls
        float cx = best->x + std::cos(a) * shift, cy = best->y + std::sin(a) * shift;
        float width = best->wl + best->wr;
        float scale = std::min((width + 0.6f) / 6.94f, (best->roof + 0.3f) / 9.39f);
        float o = std::fmod(a + 2 * float(M_PI), 2 * float(M_PI));
        handler->PSendSysMessage("gatefit: ({:.1f}, {:.1f}, {:.1f}) orientation {:.2f} scale {:.2f}  (way {:.1f} m wide, roof {:.1f} m)",
            cx, cy, best->z, o, scale, width, best->roof);
    }

    void ProbeGate(ChatHandler* handler, uint32 mapId, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        Map* map = sMapMgr->CreateBaseMap(mapId);
        if (map)
        {
            // the navmesh tiles come with the grids: load every grid the route could wander through
            float lo_x = std::min(x1, x2) - 300.f, hi_x = std::max(x1, x2) + 300.f;
            float lo_y = std::min(y1, y2) - 300.f, hi_y = std::max(y1, y2) + 300.f;
            for (float gx = lo_x; gx <= hi_x; gx += 200.f)
                for (float gy = lo_y; gy <= hi_y; gy += 200.f)
                    map->LoadGrid(gx, gy);
        }
        dtNavMeshQuery const* q = map ? map->GetMapCollisionData().GetMMapData().GetNavMeshQuery() : nullptr;
        if (!q)
        {
            handler->SendSysMessage("gateprobe: no navmesh for that map");
            return;
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);
        float a[3] = { y1, z1, x1 }, b[3] = { y2, z2, x2 }, ext[3] = { 5.f, 10.f, 5.f }, pa[3], pb[3];
        dtPolyRef ra = 0, rb = 0;
        q->findNearestPoly(a, ext, &filter, &ra, pa);
        q->findNearestPoly(b, ext, &filter, &rb, pb);
        if (!ra || !rb)
        {
            handler->SendSysMessage("gateprobe: start or end is off the navmesh");
            return;
        }
        static dtPolyRef polys[4096];
        int npolys = 0;
        q->findPath(ra, rb, pa, pb, &filter, polys, &npolys, 4096);
        static float pts[2048 * 3];
        int npts = 0;
        if (npolys)
            q->findStraightPath(pa, pb, polys, npolys, pts, nullptr, nullptr, &npts, 2048);
        if (npts < 2)
        {
            handler->SendSysMessage("gateprobe: no path");
            return;
        }
        handler->PSendSysMessage("gateprobe: {} path points", npts);
        for (int i = 0; i < npts; ++i)
            handler->PSendSysMessage("  path ({:.1f}, {:.1f}, {:.1f})", pts[i * 3 + 2], pts[i * 3], pts[i * 3 + 1]);
        uint32 lastZone = 0, lastArea = 0;
        float px = 0, py = 0, pz = 0;
        bool first = true;
        for (int i = 0; i + 1 < npts; ++i)
        {
            float sx = pts[i * 3 + 2], sy = pts[i * 3], sz = pts[i * 3 + 1];
            float ex = pts[i * 3 + 5], ey = pts[i * 3 + 3], ez = pts[i * 3 + 4];
            float len = std::sqrt((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy));
            int steps = std::max(1, int(len));
            for (int k = 0; k < steps; ++k)
            {
                float t = float(k) / steps;
                float x = sx + (ex - sx) * t, y = sy + (ey - sy) * t, z = sz + (ez - sz) * t;
                uint32 zone = 0, area = 0;
                map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, area, x, y, z + 0.5f);
                if (first || zone != lastZone || area != lastArea)
                {
                    float dir = std::atan2(y - py, x - px);
                    // the width of the way here: step out sideways until the mesh or a wall stops us
                    float wl = 0.f, wr = 0.f;
                    for (int side = -1; side <= 1; side += 2)
                    {
                        float w = 0.f;
                        for (float d = 0.5f; d <= 30.f; d += 0.5f)
                        {
                            float qx = x + std::cos(dir + side * float(M_PI) / 2) * d, qy = y + std::sin(dir + side * float(M_PI) / 2) * d;
                            float qz = Ground(map, qx, qy, z);
                            if (qz <= INVALID_HEIGHT || std::fabs(qz - z) > 3.f || MeshDistance(map, qx, qy, qz) > 1.f)
                                break;
                            w = d;
                        }
                        (side < 0 ? wl : wr) = w;
                    }
                    if (!first)
                    {
                        // a sideways profile across the way: offset, ground height step, distance off the walkable mesh
                        std::string prof;
                        for (int d = -16; d <= 16; d += 2)
                        {
                            float qx = x + std::cos(dir + float(M_PI) / 2) * d, qy = y + std::sin(dir + float(M_PI) / 2) * d;
                            float qz = Ground(map, qx, qy, z);
                            float md = qz > INVALID_HEIGHT ? MeshDistance(map, qx, qy, qz) : 99.f;
                            prof += Acore::StringFormat(" {}:{:+.1f}/{:.1f}", d, qz > INVALID_HEIGHT ? qz - z : 99.f, md);
                        }
                        handler->PSendSysMessage("  profile (offset:height/mesh){}", prof);
                    }
                    float ceil = map->GetHeight(PHASEMASK_NORMAL, x, y, z + 40.f, true, 40.f);
                    handler->PSendSysMessage("  {} zone {} area {} at ({:.1f}, {:.1f}, {:.1f}) heading {:.2f}; width {:.1f} left + {:.1f} right; "
                        "from ({:.1f}, {:.1f}, {:.1f}); ground below +40 m: {:.1f}", first ? "start" : "->", zone, area, x, y, z,
                        dir, wl, wr, px, py, pz, ceil);
                    lastZone = zone;
                    lastArea = area;
                    first = false;
                }
                // every 5 m: the real floor, the width of the way (walkable, no big step) and whether there's a roof
                static float walked = 0.f;
                if (i == 0 && k == 0)
                    walked = 0.f;
                walked += 1.f;
                if (int(walked) % 5 == 0)
                {
                    float fz = Ground(map, x, y, z + 2.f);
                    if (fz > INVALID_HEIGHT)
                    {
                        float dir = std::atan2(ey - sy, ex - sx);
                        float w[2] = { 0.f, 0.f };
                        for (int side = 0; side < 2; ++side)
                            for (float d = 1.f; d <= 25.f; d += 1.f)
                            {
                                float ang = dir + (side ? 1.f : -1.f) * float(M_PI) / 2;
                                float qx = x + std::cos(ang) * d, qy = y + std::sin(ang) * d;
                                float qz = Ground(map, qx, qy, fz + 2.f);
                                if (qz <= INVALID_HEIGHT || std::fabs(qz - fz) > 2.f || MeshDistance(map, qx, qy, qz) > 1.f)
                                    break;
                                w[side] = d;
                            }
                        bool roof = !map->isInLineOfSight(x, y, fz + 1.f, x, y, fz + 25.f, PHASEMASK_NORMAL,
                            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
                        handler->PSendSysMessage("  at ({:.1f}, {:.1f}, {:.1f}) heading {:.2f}: width {:.0f}+{:.0f}{}", x, y, fz, dir,
                            w[0], w[1], roof ? "  ROOF" : "");
                    }
                }
                px = x; py = y; pz = z;
            }
        }
        handler->PSendSysMessage("  end at ({:.1f}, {:.1f}, {:.1f})", px, py, pz);
    }
}
