/*---------------------------------------------------------------------------*\
    CFDEMcoupling - Open Source CFD-DEM coupling

    CFDEMcoupling is part of the CFDEMproject
    www.cfdem.com
                                Christoph Goniva, christoph.goniva@cfdem.com
                                Copyright 2009-2012 JKU Linz
                                Copyright 2012-     DCS Computing GmbH, Linz
-------------------------------------------------------------------------------
License
    This file is part of CFDEMcoupling.

    CFDEMcoupling is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 3 of the License, or (at your
    option) any later version.

    CFDEMcoupling is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with CFDEMcoupling; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

Description
    This code is designed to realize coupled CFD-DEM simulations using LIGGGHTS
    and OpenFOAM(R). Note: this code is not part of OpenFOAM(R) (see DISCLAIMER).
\*---------------------------------------------------------------------------*/

#include "error.H"

#include "IBVoidFraction.H"
#include "addToRunTimeSelectionTable.H"
#include "locateModel.H"
#include "dataExchangeModel.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

defineTypeNameAndDebug(IBVoidFraction, 0);

addToRunTimeSelectionTable
(
    voidFractionModel,
    IBVoidFraction,
    dictionary
);


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

// Construct from components
IBVoidFraction::IBVoidFraction
(
    const dictionary& dict,
    cfdemCloud& sm
)
:
    voidFractionModel(dict,sm),
    propsDict_(dict.subDict(typeName + "Props")),
    alphaMin_(readScalar(propsDict_.lookup("alphaMin"))),
    alphaLimited_(0),
    scaleUpVol_(readScalar(propsDict_.lookup("scaleUpVol"))),
    sqrtThree_(sqrt(3.0))
{
    Info << "\n\n W A R N I N G - do not use in combination with differentialRegion model!\n\n" << endl;
    maxCellsPerParticle_ = readLabel(propsDict_.lookup("maxCellsPerParticle"));

    if (scaleUpVol_ < 1.0)
        FatalError << "scaleUpVol shloud be > 1." << abort(FatalError);
    if (alphaMin_ > 1.0 || alphaMin_ < 0.01)
        FatalError << "alphaMin must have a value between 0.01 and 1.0." << abort(FatalError);
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

IBVoidFraction::~IBVoidFraction()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void IBVoidFraction::setvoidFraction(double** const& mask,double**& voidfractions,double**& particleWeights,double**& particleVolumes,double**& particleV)
{
    const boundBox& globalBb = particleCloud_.mesh().bounds();

    reAllocArrays();

    voidfractionNext_.ref() = 1.0;

    for (int index=0; index < particleCloud_.numberOfParticles(); index++)
    {
        //if(mask[index][0])
        //{
            //reset
            for (int subcell=0; subcell < cellsPerParticle_[index][0]; subcell++)
            {
                particleWeights[index][subcell] = 0.0;
                particleVolumes[index][subcell] = 0.0;
            }

            cellsPerParticle_[index][0]=1;
            particleV[index][0]=0;

            //collecting data
            label particleCenterCellID=particleCloud_.cellIDs()[index][0];
            scalar radius =  particleCloud_.radius(index);
            vector positionCenter = particleCloud_.position(index);

            if (particleCenterCellID >= 0)
            {
                labelHashSet hashSett;

                //compute the voidfraction for the cell "particleCentreCellID
                vector cellCentrePosition = particleCloud_.mesh().C()[particleCenterCellID];
                scalar fc = pointInParticle(index, positionCenter, cellCentrePosition);
                vector minPeriodicParticlePos = positionCenter;

                if (particleCloud_.checkPeriodicCells()) //consider minimal distance to all periodic images of this particle
                {
                    fc = minPeriodicDistance(index,cellCentrePosition, positionCenter, globalBb, minPeriodicParticlePos);
                }

                scalar centreDist = mag(cellCentrePosition - minPeriodicParticlePos);
                scalar corona = 0.5 * sqrtThree_ * pow(particleCloud_.mesh().V()[particleCenterCellID], 1./3.);
                vector coronaPoint = cellCentrePosition;
                if(centreDist > 0.0)
                  coronaPoint += (cellCentrePosition - minPeriodicParticlePos) * (corona / centreDist);

                if (pointInParticle(index, minPeriodicParticlePos, coronaPoint) < 0.0)
                {
                    voidfractionNext_[particleCenterCellID] = 0.0;
                }
                else
                {
                    const labelList& vertices = particleCloud_.mesh().cellPoints()[particleCenterCellID];
                    const double ratio = 0.125;

                    forAll(vertices, i)
                    {
                        vector vertexPosition = particleCloud_.mesh().points()[vertices[i]];
                        scalar fv = pointInParticle(index, positionCenter, vertexPosition);

                        if (particleCloud_.checkPeriodicCells()) //consider minimal distance to all periodic images of this particle
                        {
                            fv = minPeriodicDistance(index, vertexPosition, positionCenter, globalBb, minPeriodicParticlePos);
                        }

                        if (fc < 0.0 && fv < 0.0)
                        {
                            voidfractionNext_[particleCenterCellID] -= ratio;
                        }
                        else if (fc < 0.0 && fv > 0.0)
                        {
                            //compute lambda
                            scalar lambda = segmentParticleIntersection(index, minPeriodicParticlePos, cellCentrePosition, vertexPosition);
                            voidfractionNext_[particleCenterCellID] -= ratio * lambda;
                        }
                        else if (fc > 0.0 && fv < 0.0)
                        {
                            //compute lambda
                            scalar lambda = segmentParticleIntersection(index, minPeriodicParticlePos, vertexPosition, cellCentrePosition);
                            voidfractionNext_[particleCenterCellID] -= ratio * lambda;
                        }
                    }
                } //end particle partially overlapping with cell

                //generating list with cell and subcells
                buildLabelHashSet(index, minPeriodicParticlePos, particleCenterCellID, hashSett, true);

                //Add cells of periodic particle images on same processor
                if (particleCloud_.checkPeriodicCells())
                {
                    int doPeriodicImage[3];
                    for (int iDir=0; iDir < 3; iDir++)
                    {
                        doPeriodicImage[iDir] = 0;
                        if ((minPeriodicParticlePos[iDir]+radius) > globalBb.max()[iDir])
                        {
                            doPeriodicImage[iDir] = -1;
                        }
                        if ((minPeriodicParticlePos[iDir]-radius) < globalBb.min()[iDir])
                        {
                            doPeriodicImage[iDir] = 1;
                        }
                    }

                    //scan directions and map particles
                    List<vector> particlePosList;         //List of particle center position
                    List<label>  particleLabelList;

                    int copyCounter = 0;
                    particlePosList.append(minPeriodicParticlePos);

                    //x-direction
                    if (doPeriodicImage[0] != 0)
                    {
                        particlePosList.append( particlePosList[copyCounter]
                                                + vector(
                                                              static_cast<double>(doPeriodicImage[0])
                                                              *(globalBb.max()[0]-globalBb.min()[0]),
                                                              0.0,
                                                              0.0)
                                               );
                        copyCounter++;
                    }
                    //y-direction
                    int currCopyCounter = copyCounter;
                    if (doPeriodicImage[1] != 0)
                    {
                        for (int yDirCop=0; yDirCop <= currCopyCounter; yDirCop++)
                        {
                            particlePosList.append( particlePosList[yDirCop]
                                                    + vector(
                                                              0.0,
                                                              static_cast<double>(doPeriodicImage[1])
                                                              *(globalBb.max()[1]-globalBb.min()[1]),
                                                              0.0)
                                                   );
                            copyCounter++;
                        }
                    }
                    //z-direction
                    currCopyCounter = copyCounter;
                    if (doPeriodicImage[2] != 0)
                    {
                        for (int zDirCop=0; zDirCop <= currCopyCounter; zDirCop++)
                        {
                            particlePosList.append( particlePosList[zDirCop]
                                                    + vector(
                                                              0.0,
                                                              0.0,
                                                              static_cast<double>(doPeriodicImage[2])
                                                              *(globalBb.max()[2]-globalBb.min()[2])
                                                              )
                                                   );
                            copyCounter++;
                        }
                    }

                    //add the nearest cell labels
                    particleLabelList.append(particleCenterCellID);
                    for (int iPeriodicImage=1; iPeriodicImage <= copyCounter; iPeriodicImage++)
                    {
                        label partCellId = particleCloud_.mesh().findNearestCell(particlePosList[iPeriodicImage]);
                        particleLabelList.append(partCellId);

                        buildLabelHashSet(index, particlePosList[iPeriodicImage], particleLabelList[iPeriodicImage], hashSett, false);
                    }

                } //end checkPeriodicCells_


                label hashSetLength = hashSett.size();
                if (hashSetLength > maxCellsPerParticle_)
                {
                    FatalError << "big particle algo found more cells (" << hashSetLength
                               << ") than storage is prepared (" << maxCellsPerParticle_ << ")" << abort(FatalError);
                }
                else if (hashSetLength > 0)
                {
                    cellsPerParticle_[index][0]=hashSetLength;
                    hashSett.erase(particleCenterCellID);

                    for (label i=0; i < hashSetLength-1; i++)
                    {
                        label cellI = hashSett.toc()[i];
                        particleCloud_.cellIDs()[index][i+1]=cellI; //adding subcell represenation
                    }
                }//end cells found on this proc
            }// end found cells
        //}// end if masked
    }// end loop all particles

    for (label index=0; index < particleCloud_.numberOfParticles(); index++)
    {
        for (label subcell=0; subcell < cellsPerParticle_[index][0]; subcell++)
        {
            label cellID = particleCloud_.cellIDs()[index][subcell];

            if(cellID >= 0)
            {
                if (voidfractionNext_[cellID] < 0.0)
                    voidfractionNext_[cellID] = 0.0;
                 voidfractions[index][subcell] = voidfractionNext_[cellID];
            }
            else
            {
                voidfractions[index][subcell] = -1.;
            }
        }
    }
}

void IBVoidFraction::buildLabelHashSet
(
    int index,
    const vector position,
    const label cellID,
    labelHashSet& hashSett,
    bool initialInsert //initial insertion of own cell
)
{
    if(initialInsert)
        hashSett.insert(cellID);

    const labelList& nc = particleCloud_.mesh().cellCells()[cellID];
    forAll(nc,i)
    {
        label neighbor=nc[i];
        vector cellCentrePosition = particleCloud_.mesh().C()[neighbor];
        scalar centreDist = mag(cellCentrePosition-position);

        scalar fc = pointInParticle(index, position, cellCentrePosition);
        scalar corona = 0.5 * sqrtThree_ * pow(particleCloud_.mesh().V()[neighbor], 1./3.);
        vector coronaPoint = cellCentrePosition;
        if (centreDist > 0.0)
            coronaPoint += (cellCentrePosition - position) * (corona / centreDist);

        if (!hashSett.found(neighbor) && pointInParticle(index, position, coronaPoint) < 0.0)
        {
            voidfractionNext_[neighbor] = 0.;
            buildLabelHashSet(index,position,neighbor,hashSett,true);
        }
        else if(!hashSett.found(neighbor))
        {
            scalar scale = 1.;
            const labelList& vertexPoints = particleCloud_.mesh().cellPoints()[neighbor];
            const scalar ratio = 0.125;

            forAll(vertexPoints, j)
            {
                vector vertexPosition = particleCloud_.mesh().points()[vertexPoints[j]];
                scalar fv = pointInParticle(index, position, vertexPosition);

                if (fc < 0.0)
                {
                    if (fv < 0.0)
                    {
                        scale -= ratio;
                    }
                    else
                    {
                        scalar lambda = segmentParticleIntersection(index, position, cellCentrePosition, vertexPosition);
                        scale -= lambda * ratio;
                    }
                }
                else if (fv < 0.0)
                {
                    scalar lambda = segmentParticleIntersection(index, position, vertexPosition, cellCentrePosition);
                    scale -= lambda * ratio;
                }
            }

            if (scale < 0.0)
                scale = 0.0;

            if (voidfractionNext_[neighbor] == 1.0)
            {
                voidfractionNext_[neighbor] = scale;
            }
            else
            {
                voidfractionNext_[neighbor] -= (1.0-scale);
                if (voidfractionNext_[neighbor] < 0.)
                    voidfractionNext_[neighbor] = 0.0;
            }

            if (!(scale == 1.0))
                buildLabelHashSet(index,position,neighbor,hashSett, true);
        }
    }
}

double IBVoidFraction::segmentParticleIntersection(int index, vector positionCenter, vector pointInside, vector pointOutside) const
{
    const scalar radius = particleCloud_.radius(index);
    const scalar a = (pointOutside - pointInside) & (pointOutside - pointInside);
    const scalar b = 2.*(pointOutside - pointInside) & (pointInside - positionCenter);
    const scalar c = ((pointInside - positionCenter) & (pointInside - positionCenter)) - radius*radius;
    const scalar D = b*b - 4.0*a*c;
    const scalar eps = 1e-12;
    scalar lambda_ = 0.0;
    scalar lambda = 0.0;

    if (D >= 0.0)
    {
        scalar sqrtD = sqrt(D);
        lambda_ = (-b + sqrtD)/(2.0*a);

        if (lambda_ >= -eps && lambda_ <= 1.0+eps)
        {
            lambda = lambda_;
        }
        else
        {
            lambda_ = (-b - sqrtD)/(2.0*a);
            if (lambda_ >= -eps && lambda_ <= 1.0+eps)
                lambda = lambda_;
        }
    }

    if (lambda < 0.0)
        return 0.0;
    else if (lambda > 1.0)
        return 1.0;

    return lambda;
}

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace Foam

// ************************************************************************* //
