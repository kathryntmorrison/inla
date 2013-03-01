##! \name{link}
##! \alias{link}
##! \alias{inla.link}
##! \alias{inla.link.log}
##! \alias{inla.link.invlog}
##! \alias{inla.link.logit}
##! \alias{inla.link.invlogit}
##! \alias{inla.link.probit}
##! \alias{inla.link.invprobit}
##! \alias{inla.link.cloglog}
##! \alias{inla.link.invcloglog}
##! \alias{inla.link.tan}
##! \alias{inla.link.invtan}
##! 
##! \title{Link functions in INLA}
##! 
##! \description{Define link-functions and its inverse}
##!
##! \usage{
##! inla.link.log(x, inverse=FALSE)
##! inla.link.invlog(x, inverse=FALSE)
##! inla.link.logit(x, inverse=FALSE)
##! inla.link.invlogit(x, inverse=FALSE)
##! inla.link.probit(x, inverse=FALSE)
##! inla.link.invprobit(x, inverse=FALSE)
##! inla.link.cloglog(x, inverse=FALSE)
##! inla.link.invcloglog(x, inverse=FALSE)
##! inla.link.tan(x, inverse=FALSE)
##! inla.link.invtan(x, inverse=FALSE)
##! }
##! 
##! \arguments{
##!     \item{x}{The argument. A numeric vector.}
##!     \item{inverse}{Logical. Use the link (\code{inverse=FALSE})
##!                    or its inverse (\code{inverse=TRUE})}
##!}
##! 
##! \value{Return the values of the link-function or its inverse.}
##! \note{The \code{inv}-functions are redundant,  as 
##!       \code{inla.link.invlog(x) = inla.link.log(x, inverse=TRUE)}
##!       and so on,  but they are simpler to use a arguments
##!       to other functions.}
##! \author{Havard Rue \email{hrue@math.ntnu.no}}
##! 
##! \examples{
##!}

`inla.link.log` = function(x, inverse = FALSE)
{
    if (!inverse) {
        return (log(x))
    } else {
        return (exp(x))
    }
}
`inla.link.invlog` = function(x, inverse = FALSE)
{
    return (inla.link.log(x, inverse = !inverse))
}

`inla.link.logit` = function(x, inverse = FALSE)
{
    if (!inverse) {
        return (log(x/(1.0-x)))
    } else {
        return (1.0/(1.0+exp(-x)))
    }
}
`inla.link.invlogit` = function(x, inverse = FALSE)
{
    return (inla.link.logit(x, inverse = !inverse))
}

`inla.link.probit` = function(x, inverse = FALSE)
{
    if (!inverse) {
        return (qnorm(x))
    } else {
        return (pnorm(x))
    }
}
`inla.link.invprobit` = function(x, inverse = FALSE)
{
    return (inla.link.probit(x, inverse = !inverse))
}

`inla.link.cloglog` = function(x, inverse = FALSE)
{
    if (!inverse) {
        return (-log(-log(x)))
    } else {
        return (exp(-exp(-x)))
    }
}
`inla.link.invcloglog` = function(x, inverse = FALSE)
{
    return (inla.link.cloglog(x, inverse = !inverse))
}

`inla.link.tan` = function(x, inverse = TRUE)
{
    if (!inverse) {
        return (tan(x/2.0))
    } else {
        return (2.0*atan(x))
    }
}
`inla.link.invtan` = function(x, inverse = FALSE)
{
    return (inla.link.tan(x, inverse = !inverse))
}