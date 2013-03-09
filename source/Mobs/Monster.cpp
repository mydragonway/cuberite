
#include "Globals.h"  // NOTE: MSVC stupidness requires this to be the same across all modules

#include "Monster.h"
#include "../Root.h"
#include "../Server.h"
#include "../ClientHandle.h"
#include "../World.h"
#include "../Player.h"
#include "../Defines.h"
#include "../MonsterConfig.h"
#include "../MersenneTwister.h"

#include "../Vector3f.h"
#include "../Vector3i.h"
#include "../Vector3d.h"

#include "../Tracer.h"
// #include "../../iniFile/iniFile.h"





cMonster::cMonster(const AString & a_ConfigName, char a_ProtocolMobType, const AString & a_SoundHurt, const AString & a_SoundDeath)
	: super(etMob)
	, m_Target(NULL)
	, m_bMovingToDestination(false)
	, m_DestinationTime( 0 )
	, m_Gravity( -9.81f)
	, m_bOnGround( false )
	, m_DestroyTimer( 0 )
	, m_Jump(0)
	, m_MobType(a_ProtocolMobType)
	, m_SoundHurt(a_SoundHurt)
	, m_SoundDeath(a_SoundDeath)
	, m_EMState(IDLE)
	, m_SightDistance(25)
	, m_SeePlayerInterval (0)
	, m_EMPersonality(AGGRESSIVE)
	, m_AttackDamage(1.0f)
	, m_AttackRange(5.0f)
	, m_AttackInterval(0)
	, m_AttackRate(3)
	, idle_interval(0)
{
	if (!a_ConfigName.empty())
	{
		GetMonsterConfig(a_ConfigName);
	}
}





void cMonster::SpawnOn(cClientHandle & a_Client)
{
	a_Client.SendSpawnMob(*this);
}





void cMonster::MoveToPosition( const Vector3f & a_Position )
{
	m_bMovingToDestination = true;

	m_Destination = a_Position;
}





bool cMonster::ReachedDestination()
{
	Vector3f Distance = (m_Destination) - Vector3f( m_Pos );
	if( Distance.SqrLength() < 2.f )
		return true;

	return false;
}





void cMonster::Tick(float a_Dt, MTRand & a_TickRandom)
{
	super::Tick(a_Dt, a_TickRandom);

	if (m_Health <= 0)
	{
		// The mob is dead, but we're still animating the "puff" they leave when they die
		m_DestroyTimer += a_Dt / 1000;
		if (m_DestroyTimer > 1)
		{
			Destroy();
		}
		return;
	}

	a_Dt /= 1000;

	if (m_bMovingToDestination)
	{
		Vector3f Pos( m_Pos );
		Vector3f Distance = m_Destination - Pos;
		if( !ReachedDestination() )
		{
			Distance.y = 0;
			Distance.Normalize();
			Distance *= 3;
			m_Speed.x = Distance.x;
			m_Speed.z = Distance.z;

			if (m_EMState == ESCAPING)
			{	//Runs Faster when escaping :D otherwise they just walk away
				m_Speed.x *= 2.f;
				m_Speed.z *= 2.f;
			}
		}
		else
		{
			m_bMovingToDestination = false;
		}

		if( m_Speed.SqrLength() > 0.f )
		{
			if( m_bOnGround )
			{
				Vector3f NormSpeed = m_Speed.NormalizeCopy();
				Vector3f NextBlock = Vector3f( m_Pos ) + NormSpeed;
				double NextHeight = (double)GetWorld()->GetHeight( (int)NextBlock.x, (int)NextBlock.z );
				if( NextHeight > m_Pos.y - 1.2 && NextHeight - m_Pos.y < 2.5 )
				{
					m_bOnGround = false;
					m_Speed.y = 7.f; // Jump!!
				}
			}
		}
	}

	ReplicateMovement();
	
	Vector3d Distance = m_Destination - Vector3f( m_Pos );
	if (Distance.SqrLength() > 0.1f)
	{
		double Rotation, Pitch;
		Distance.Normalize();
		VectorToEuler( Distance.x, Distance.y, Distance.z, Rotation, Pitch );
		SetRotation( Rotation );
		SetPitch( Pitch );
	}
	
	switch (m_EMState)
	{
		case IDLE:
		{
			// If enemy passive we ignore checks for player visibility
			InStateIdle(a_Dt, a_TickRandom);
			break;
		}
	
		case CHASING:
		{
			// If we do not see a player anymore skip chasing action
			InStateChasing(a_Dt, a_TickRandom);
			break;
		}
	
		case ESCAPING:
		{
			InStateEscaping(a_Dt, a_TickRandom);
			break;
		}
	}  // switch (m_EMState)
}





void cMonster::ReplicateMovement()
{
	if (m_bDirtyOrientation && !m_bDirtyPosition)
	{
		m_World->BroadcastEntLook(*this);
		m_bDirtyOrientation = false;
	}
	
	if (m_bDirtyPosition)
	{
		float DiffX = (float)(GetPosX() - m_LastPosX);
		float DiffY = (float)(GetPosY() - m_LastPosY);
		float DiffZ = (float)(GetPosZ() - m_LastPosZ);
		float SqrDist = DiffX * DiffX + DiffY * DiffY + DiffZ * DiffZ;
		if (
			(SqrDist > 4 * 4)  // 4 blocks is max Relative Move
			|| (m_World->GetWorldAge() - m_TimeLastTeleportPacket > 40)  // Send an absolute position every 2 seconds
		)
		{
			// LOGD("Teleported %f", sqrtf(SqrDist) );
			m_World->BroadcastTeleportEntity(*this);
			m_TimeLastTeleportPacket = m_World->GetWorldAge();
		}
		else
		{
			// Relative move sucks balls! It's always wrong wtf!
			if (m_bDirtyOrientation)
			{
				m_World->BroadcastEntRelMoveLook(*this, (char)(DiffX * 32), (char)(DiffY * 32), (char)(DiffZ * 32));
				m_bDirtyOrientation = false;
			}
			else
			{
				m_World->BroadcastEntRelMove(*this, (char)(DiffX * 32), (char)(DiffY * 32), (char)(DiffZ * 32));
			}
		}
		m_LastPosX = GetPosX();
		m_LastPosY = GetPosY();
		m_LastPosZ = GetPosZ();
		m_bDirtyPosition = false;
	}

	MoveToCorrectChunk();
}





void cMonster::HandlePhysics(float a_Dt)
{
	if( m_bOnGround ) // check if it's still on the ground
	{
		cWorld* World = GetWorld();
		if( World->GetBlock( (int)m_Pos.x, (int)m_Pos.y -1, (int)m_Pos.z ) == E_BLOCK_AIR )
		{
			m_bOnGround = false;
		}
		if( World->GetBlock( (int)m_Pos.x, (int)m_Pos.y, (int)m_Pos.z ) != E_BLOCK_AIR ) // If in ground itself, push it out
		{
			m_bOnGround = true;
			m_Pos.y += 0.2;
			m_bDirtyPosition = true;
		}
		m_Speed.x *= 0.7f/(1+a_Dt);
		if( fabs(m_Speed.x) < 0.05 ) m_Speed.x = 0;
		m_Speed.z *= 0.7f/(1+a_Dt);
		if( fabs(m_Speed.z) < 0.05 ) m_Speed.z = 0;
	}

	if( !m_bOnGround )
	{
		float Gravity = -9.81f*a_Dt;
		m_Speed.y += Gravity;
	}

	if( m_Speed.SqrLength() > 0.f )
	{
		cTracer Tracer( GetWorld() );
		int Ret = Tracer.Trace( m_Pos, m_Speed, 2 );
		if( Ret ) // Oh noez! we hit something
		{
			// Set to hit position
			if( (Tracer.RealHit - Vector3f(m_Pos)).SqrLength() <= ( m_Speed * a_Dt ).SqrLength() )
			{
				if( Ret == 1 )
				{

					if( Tracer.HitNormal.x != 0.f ) m_Speed.x = 0.f;
					if( Tracer.HitNormal.y != 0.f ) m_Speed.y = 0.f;
					if( Tracer.HitNormal.z != 0.f ) m_Speed.z = 0.f;

					if( Tracer.HitNormal.y > 0 ) // means on ground
					{
						m_bOnGround = true;
					}
				}
				m_Pos = Tracer.RealHit;
				m_Pos += Tracer.HitNormal * 0.2f;

			}
			else
				m_Pos += m_Speed*a_Dt;
		}
		else
		{	// We didn't hit anything, so move =]
			m_Pos += m_Speed*a_Dt;
		}

		m_bDirtyPosition = true;
	}
}





void cMonster::DoTakeDamage(TakeDamageInfo & a_TDI)
{
	super::DoTakeDamage(a_TDI);
	if((m_SoundHurt != "") && (m_Health > 0)) m_World->BroadcastSoundEffect(m_SoundHurt, (int)(m_Pos.x * 8), (int)(m_Pos.y * 8), (int)(m_Pos.z * 8), 1.0f, 0.8f);
	if (a_TDI.Attacker != NULL)
	{
		m_Target = a_TDI.Attacker;
		AddReference(m_Target);
	}
}





void cMonster::KilledBy(cPawn * a_Killer)
{
	super::KilledBy(a_Killer);
	if(m_SoundHurt != "") m_World->BroadcastSoundEffect(m_SoundDeath, (int)(m_Pos.x * 8), (int)(m_Pos.y * 8), (int)(m_Pos.z * 8), 1.0f, 0.8f);
	m_DestroyTimer = 0;
}





//----State Logic

const char *cMonster::GetState()
{
	switch(m_EMState)
	{
		case IDLE:      return "Idle";
		case ATTACKING: return "Attacking";
		case CHASING:   return "Chasing";
		default:        return "Unknown";
	}
}





// for debugging
void cMonster::SetState(const AString & a_State)
{
	if (a_State.compare("Idle") == 0)
	{
		m_EMState = IDLE;
	}
	else if (a_State.compare("Attacking") == 0)
	{
		m_EMState = ATTACKING;
	}
	else if (a_State.compare("Chasing") == 0)
	{
		m_EMState = CHASING;
	}
	else
	{
		LOGD("cMonster::SetState(): Invalid state");
		ASSERT(!"Invalid state");
	}
}





//Checks to see if EventSeePlayer should be fired
//monster sez: Do I see the player
void cMonster::CheckEventSeePlayer(MTRand & a_TickRandom)
{
	// TODO: Rewrite this to use cWorld's DoWithPlayers()
	cPlayer * Closest = FindClosestPlayer();

	if (Closest != NULL)
	{
		EventSeePlayer(Closest, a_TickRandom);
	}
}





void cMonster::CheckEventLostPlayer(MTRand & a_TickRandom)
{
	UNUSED(a_TickRandom);
	
	Vector3f pos;
	cTracer LineOfSight(GetWorld());
	
	if (m_Target != NULL)
	{
		pos = m_Target->GetPosition();
		if ((pos - m_Pos).Length() > m_SightDistance || LineOfSight.Trace(m_Pos,(pos - m_Pos), (int)(pos - m_Pos).Length()))
		{
			EventLosePlayer();
		}
	}
	else
	{
		EventLosePlayer();
	}
}





// What to do if player is seen
// default to change state to chasing
void cMonster::EventSeePlayer(cEntity * a_SeenPlayer, MTRand & a_TickRandom)
{
	UNUSED(a_TickRandom);
	
	m_Target = a_SeenPlayer;
	AddReference( m_Target );
}





void cMonster::EventLosePlayer(void)
{
	Dereference(m_Target);
	m_Target = 0;
	m_EMState = IDLE;
}





// What to do if in Idle State
void cMonster::InStateIdle(float a_Dt, MTRand & a_TickRandom)
{
	idle_interval += a_Dt;
	if (idle_interval > 1)
	{
		// at this interval the results are predictable
		int rem = (a_TickRandom.randInt() % 6) + 1;
		// LOGD("Moving: int: %3.3f rem: %i",idle_interval,rem);
		idle_interval -= 1;		// So nothing gets dropped when the server hangs for a few seconds
		Vector3f Dist;
		Dist.x = (float)((a_TickRandom.randInt() % 11) - 5);
		Dist.z = (float)((a_TickRandom.randInt() % 11) - 5);
		if ((Dist.SqrLength() > 2)  && (rem >= 3))
		{
			m_Destination.x = (float)(m_Pos.x + Dist.x);
			m_Destination.z = (float)(m_Pos.z + Dist.z);
			m_Destination.y = (float)GetWorld()->GetHeight((int)m_Destination.x, (int)m_Destination.z) + 1.2f;
			MoveToPosition(m_Destination);
		}
	}
}





// What to do if in Chasing State
// This state should always be defined in each child class
void cMonster::InStateChasing(float a_Dt, MTRand & a_TickRandom)
{
	UNUSED(a_Dt);
	UNUSED(a_TickRandom);
}





// What to do if in Escaping State
void cMonster::InStateEscaping(float a_Dt, MTRand & a_TickRandom)
{
	UNUSED(a_Dt);
	UNUSED(a_TickRandom);
	
	if (m_Target != NULL)
	{
		Vector3d newloc = m_Pos;
		newloc.x = (m_Target->GetPosition().x < newloc.x)? (newloc.x + m_SightDistance): (newloc.x - m_SightDistance);
		newloc.z = (m_Target->GetPosition().z < newloc.z)? (newloc.z + m_SightDistance): (newloc.z - m_SightDistance);
		MoveToPosition(newloc);
	}
	else
	{
		m_EMState = IDLE;  // This shouldnt be required but just to be safe
	}
}





// Do attack here
// a_Dt is passed so we can set attack rate
void cMonster::Attack(float a_Dt)
{
	m_AttackInterval += a_Dt * m_AttackRate;
	if ((m_Target != NULL) && (m_AttackInterval > 3.0))
	{
		// Setting this higher gives us more wiggle room for attackrate
		m_AttackInterval = 0.0;
		((cPawn *)m_Target)->TakeDamage(*this);
	}
}





// Checks for Players close by and if they are visible return the closest
cPlayer * cMonster::FindClosestPlayer(void)
{
	return m_World->FindClosestPlayer(m_Pos, m_SightDistance);
}





void cMonster::GetMonsterConfig(const AString & a_Name)
{
	cRoot::Get()->GetMonsterConfig()->AssignAttributes(this, a_Name);
}





void cMonster::SetAttackRate(int ar)
{
	m_AttackRate = (float)ar;
}





void cMonster::SetAttackRange(float ar)
{
	m_AttackRange = ar;
}





void cMonster::SetAttackDamage(float ad)
{
	m_AttackDamage = ad;
}





void cMonster::SetSightDistance(float sd)
{
	m_SightDistance = sd;
}





void cMonster::AddRandomDropItem(cItems & a_Drops, unsigned int a_Min, unsigned int a_Max, short a_Item, short a_ItemHealth)
{
	MTRand r1;
	int Count = r1.randInt() % (a_Max + 1 - a_Min) + a_Min;
	if (Count > 0)
	{
		a_Drops.push_back(cItem(a_Item, Count, a_ItemHealth));
	}
}




